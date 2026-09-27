# Render Path and Measurement (ROADMAP 3A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure today's render pipeline, then render the sensor as UE's primary view (TSR, one scene render per frame, Cesium LOD transitions, origin shift) and tune hitches and Cesium SSE against the measurements, on macOS.

**Architecture:** A per-frame JSONL stats recorder and a `/snapshot` endpoint in CamSim feed a Python bench harness (`scripts/bench/`) that flies a deterministic CIGI scenario and writes per-phase results and reference PNGs. The render switch keeps `SceneCapture` as the pose/FOV/post-process holder, adds a `UCameraComponent` child as the player view target, and grabs the final frame with a scene view extension into the existing readback ring, so the CPU sensor model, encoder and KLV are untouched. `render.view_source: scene_capture` keeps the old path for A/B runs.

**Tech Stack:** UE 5.8 C++ (scene view extensions, RDG, HTTPServer, ImageWrapper), Cesium for Unreal 2.29.1 (`UCesiumOriginShiftComponent`, `ACesium3DTileset`), Python 3.10+ with `uv` (pytest, numpy, pillow), bash.

**Spec:** `docs/superpowers/specs/2026-09-26-render-path-design.md`

## Global Constraints

- Platform for this plan: macOS, Apple M1 Pro, Metal. Nothing may be Metal-specific: code must also compile and behave on Linux/Vulkan (NVIDIA) and in the Docker image.
- UE targets stay on `BuildSettingsVersion.V7`. Cesium stays in the engine, never in `Plugins/`.
- Every new file starts with `// Copyright CamSim Contributors. All Rights Reserved.`; `#include "CoreMinimal.h"` first, `.generated.h` last; forward declarations in headers.
- UE naming (`A`/`U`/`F`/`E`/`I`), PascalCase, verb-first functions.
- Every new config key has a YAML key, a `CAMSIM_*` env override, an entry in `deploy/camsim_config.yaml`, and a row in `docs/configuration.md`. The canonical config must have no unknown keys (`CamSim.Config.CanonicalConfigHasNoUnknownKeys`).
- Altitudes in CIGI packets are WGS-84 ellipsoid heights.
- 1 UE unit = 1 cm.
- `TAtomic::Load/Store` take `EMemoryOrder`; `Exchange` is single-arg.
- RHI readback calls are render-thread only.
- HTTP automation tests tick both `FHttpModule::Get().GetHttpManager()` and `FTSTicker::GetCoreTicker()`.
- Headless tests on macOS need `-DisablePython`.
- Don't pipe `run.sh` through `tee` without `set -o pipefail`.
- Commits end with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
  ```

## Commands used throughout

```bash
# Build (from repo root)
scripts/run.sh --build-only

# Headless automation tests on macOS (filter by replacing CamSim with e.g. CamSim.Render)
UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
"$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim+Quit" -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$PWD/.cache/automation-report" \
  -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput
python3 scripts/parse_automation_report.py .cache/automation-report/index.json

# Python tests
uv run --with pytest --with numpy --with pillow pytest scripts/tests/ -v
```

If `parse_automation_report.py` takes different arguments, run it with `--help` first and use what it says. The report is UTF-8 with a BOM (`encoding="utf-8-sig"`).

## Review Focus

1. **Snapshot requested while the sensor is off or the terrain gate is closed.** No frame is ever grabbed, so the client must get a 503 after the timeout, not a hung socket. Pinned in Task 3 (`CamSim.Health.Snapshot.TimesOutWithoutFrame`).
2. **Viewport size or format differs from the capture size** (Retina scale on a windowed Mac run, or an HDR/10-bit backbuffer). The grab must scale or convert, never read out of bounds or hand the encoder a wrong-sized buffer. Pinned in Task 7 (grab target is always capture size; the smoke check in Task 10 asserts snapshot dimensions).
3. **Frame-stats path in a directory that doesn't exist, or unwritable.** CamSim must log once and carry on with stats off, not crash or spam. Pinned in Task 2 (`CamSim.Render.FrameStats.OpenFailureIsSoft`).
4. **CIGI teleport and origin rebase in the same frame, or a rebase with no teleport.** Both move the camera's UE location by kilometres. Each must produce exactly one camera cut and no false terrain-gate teleport. Pinned in Task 8 (`CamSim.Render.CameraCut.Thresholds` covers large translations of either origin) and exercised by the `far_origin` phase.
5. **`view_source` typo in config or env** (for example `primery`). This must warn and fall back to `primary`, the same as the encoder preference. Pinned in Task 1 (`CamSim.Config.RenderSection` bad-value case).

---

## File map

| File | Status | Responsibility |
| ---- | ------ | -------------- |
| `Source/CamSimTest/Config/CamSimConfig.{h,cpp}` | modify | `FRenderConfig`, `Operational.bSnapshotEndpointEnabled`, `Operational.FrameStatsPath` |
| `Source/CamSimTest/Camera/CamSimFrameStats.{h,cpp}` | create | Stats sample, JSON row formatting, JSONL recorder, view-family counter extension |
| `Source/CamSimTest/Health/CamSimSnapshotService.{h,cpp}` | create | Pending snapshot requests, PNG encode, timeouts |
| `Source/CamSimTest/Health/CamSimHealthServer.{h,cpp}` | modify | Optional `/snapshot` route |
| `Source/CamSimTest/Camera/FrameGrabRequestQueue.h` | create | Pure FIFO of grab requests (render-thread owned) |
| `Source/CamSimTest/Camera/CamSimFrameGrabExtension.{h,cpp}` | create | Scene view extension that copies the game view's final image into the grab target |
| `Source/CamSimTest/Camera/CamSimRenderPath.{h,cpp}` | create | Pure helpers: camera-cut test, view-source name |
| `Source/CamSimTest/Camera/CamSimCamera.{h,cpp}` | modify | Camera component, view target, stats wiring, camera cuts, origin shift |
| `Source/CamSimTest/Camera/CamSimCaptureComponent.{h,cpp}` | modify | Primary-mode capture via grab requests; show flags; AA; snapshot hand-off |
| `Source/CamSimTest/Camera/CamSimStreamingController.{h,cpp}` | modify | Skip the primary Cesium camera slot in primary mode |
| `Source/CamSimTest/Subsystem/CamSimSubsystem.{h,cpp}` | modify | Own the snapshot service, bind the route |
| `Source/CamSimTest/Tests/RenderConfigTest.cpp` | create | Config tests |
| `Source/CamSimTest/Tests/RenderPathTest.cpp` | create | Frame stats, grab queue, camera cut tests |
| `Source/CamSimTest/Tests/SnapshotEndpointTest.cpp` | create | `/snapshot` HTTP tests |
| `scripts/send_cigi_test.py` | modify | Add `pack_art_part_control` |
| `scripts/bench/{scenario,run_bench,analyze,compare}.py` | create | Harness |
| `scripts/bench/README.md` | create | How to run and read the bench |
| `scripts/tests/test_bench_*.py` | create | Harness tests |
| `scripts/ci_validate.sh` | modify | Real-RHI smoke |
| `deploy/camsim_config.yaml`, `docs/configuration.md` | modify | New keys |
| `.gitattributes` | modify | LFS for bench shots |
| `ROADMAP.md`, `CLAUDE.md` | modify | Progress, test count, commands |

All C++ paths below are relative to `unreal_project/CamSimTest/Source/CamSimTest/`.

---

### Task 1: Render, snapshot and frame-stats config keys

**Files:**
- Modify: `Config/CamSimConfig.h` (add `FRenderConfig` after `FOperationalConfig`, around line 792; add two fields to `FOperationalConfig`)
- Modify: `Config/CamSimConfig.cpp` (parser next to `ParseEncoderPreference`, around line 83; YAML in the `operational` block, around line 1366; env next to the Phase 28 operational overrides, around line 1760)
- Modify: `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `Tests/RenderConfigTest.cpp` (create)

**Interfaces:**
- Produces:
  - `FCamSimConfig::FRenderConfig` with `enum class EViewSource : uint8 { Primary, SceneCapture }`, `FString ViewSource = TEXT("primary")`, `EViewSource ViewSourceMode = EViewSource::Primary`, `float CameraCutDistanceM = 500.0f`, `float CameraCutAngleDeg = 30.0f`, `double OriginShiftDistanceM = 0.0`, and `bool IsPrimary() const`.
  - `FCamSimConfig::Render`.
  - `FCamSimConfig::FOperationalConfig::bSnapshotEndpointEnabled = false`, `FString FrameStatsPath` (empty = off).

- [ ] **Step 1: Write the failing test**

Create `Tests/RenderConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// ROADMAP 3A: render path, snapshot endpoint and frame-stats settings reach
// FCamSimConfig from YAML and from env vars; a bad view_source falls back.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigSectionTest,
	"CamSim.Config.RenderSection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigSectionTest::RunTest(const FString& Parameters)
{
	using EViewSource = FCamSimConfig::FRenderConfig::EViewSource;

	const FCamSimConfig D;
	TestTrue(TEXT("default view source is primary"), D.Render.IsPrimary());
	TestEqual(TEXT("origin shift off by default"), D.Render.OriginShiftDistanceM, 0.0);
	TestFalse(TEXT("snapshot endpoint off by default"), D.Operational.bSnapshotEndpointEnabled);
	TestTrue(TEXT("frame stats off by default"), D.Operational.FrameStatsPath.IsEmpty());

	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"render:\n"
		"  view_source: scene_capture\n"
		"  camera_cut_distance_m: 250.0\n"
		"  camera_cut_angle_deg: 15.0\n"
		"  origin_shift_distance_m: 20000.0\n"
		"operational:\n"
		"  snapshot_endpoint_enabled: true\n"
		"  frame_stats_path: \"/tmp/camsim-frames.jsonl\"\n"));
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestTrue(TEXT("scene_capture parsed"), Cfg.Render.ViewSourceMode == EViewSource::SceneCapture);
	TestFalse(TEXT("IsPrimary false"), Cfg.Render.IsPrimary());
	TestEqual(TEXT("cut distance"), Cfg.Render.CameraCutDistanceM, 250.0f);
	TestEqual(TEXT("cut angle"), Cfg.Render.CameraCutAngleDeg, 15.0f);
	TestEqual(TEXT("origin shift distance"), Cfg.Render.OriginShiftDistanceM, 20000.0);
	TestTrue(TEXT("snapshot enabled"), Cfg.Operational.bSnapshotEndpointEnabled);
	TestEqual(TEXT("frame stats path"), Cfg.Operational.FrameStatsPath, FString(TEXT("/tmp/camsim-frames.jsonl")));

	AddExpectedMessage(TEXT("Unknown render.view_source 'primery'"), EAutomationExpectedErrorFlags::Contains, 1);
	const FCamSimConfig Bad = FCamSimConfig::LoadFromYamlString(TEXT("render:\n  view_source: primery\n"));
	TestTrue(TEXT("typo falls back to primary"), Bad.Render.IsPrimary());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigEnvTest,
	"CamSim.Config.RenderEnvOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigEnvTest::RunTest(const FString& Parameters)
{
	struct FEnv { const TCHAR* Key; const TCHAR* Value; };
	const FEnv Vars[] = {
		{ TEXT("CAMSIM_RENDER_VIEW_SOURCE"),             TEXT("scene_capture") },
		{ TEXT("CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M"),   TEXT("123") },
		{ TEXT("CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG"),    TEXT("12") },
		{ TEXT("CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M"), TEXT("5000") },
		{ TEXT("CAMSIM_SNAPSHOT_ENDPOINT_ENABLED"),      TEXT("1") },
		{ TEXT("CAMSIM_FRAME_STATS_PATH"),               TEXT("/tmp/x.jsonl") },
	};
	for (const FEnv& V : Vars) { FPlatformMisc::SetEnvironmentVar(V.Key, V.Value); }

	FCamSimConfig Cfg;
	FCamSimConfig::ApplyEnvOverrides(Cfg);

	for (const FEnv& V : Vars) { FPlatformMisc::SetEnvironmentVar(V.Key, TEXT("")); }

	TestFalse(TEXT("env view source"), Cfg.Render.IsPrimary());
	TestEqual(TEXT("env cut distance"), Cfg.Render.CameraCutDistanceM, 123.0f);
	TestEqual(TEXT("env cut angle"), Cfg.Render.CameraCutAngleDeg, 12.0f);
	TestEqual(TEXT("env origin shift"), Cfg.Render.OriginShiftDistanceM, 5000.0);
	TestTrue(TEXT("env snapshot"), Cfg.Operational.bSnapshotEndpointEnabled);
	TestEqual(TEXT("env frame stats"), Cfg.Operational.FrameStatsPath, FString(TEXT("/tmp/x.jsonl")));
	return true;
}
```

Before relying on it, check how `GetEnv*` treats an empty string: read `Config/CamSimConfig.cpp:29-63`. If an empty value is not treated as "unset", clear the variables another way that the existing env tests use (`grep -n SetEnvironmentVar Tests/*.cpp`), and copy that pattern.

- [ ] **Step 2: Run the test to verify it fails**

Run: `scripts/run.sh --build-only`
Expected: compile error, `no member named 'Render' in 'FCamSimConfig'`.

- [ ] **Step 3: Add the config fields**

In `Config/CamSimConfig.h`, inside `FOperationalConfig` after `HealthHttpPort`:

```cpp
		// 3A: GET /snapshot returns the next grabbed frame as PNG (bench harness).
		bool    bSnapshotEndpointEnabled = false;
		// 3A: per-frame JSONL render stats for the bench harness (empty = disabled).
		FString FrameStatsPath;
```

After `FOperationalConfig Operational;`:

```cpp
	// ROADMAP 3A — render path
	struct FRenderConfig
	{
		enum class EViewSource : uint8
		{
			Primary = 0,   // the sensor is the game viewport's view (TSR, one render)
			SceneCapture,  // legacy SceneCapture2D path, kept for A/B until 3B
		};

		// primary | scene_capture. Env: CAMSIM_RENDER_VIEW_SOURCE
		FString     ViewSource     = TEXT("primary");
		EViewSource ViewSourceMode = EViewSource::Primary;

		// Pose jumps above either threshold in one frame reset TSR history.
		// Env: CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M / CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG
		float CameraCutDistanceM = 500.0f;
		float CameraCutAngleDeg  = 30.0f;

		// Rebase the Cesium georeference when the camera is this far from the
		// origin, in metres. 0 = disabled. Env: CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M
		double OriginShiftDistanceM = 0.0;

		bool IsPrimary() const { return ViewSourceMode == EViewSource::Primary; }
	};
	FRenderConfig Render;
```

- [ ] **Step 4: Parse YAML and env**

In `Config/CamSimConfig.cpp`, after `ParseEncoderPreference`:

```cpp
static FCamSimConfig::FRenderConfig::EViewSource ParseViewSource(const FString& Value)
{
	using EViewSource = FCamSimConfig::FRenderConfig::EViewSource;
	const FString Lower = Value.ToLower().TrimStartAndEnd();
	if (Lower == TEXT("scene_capture")) return EViewSource::SceneCapture;
	if (Lower == TEXT("primary") || Lower.IsEmpty()) return EViewSource::Primary;
	UE_LOG(LogCamSim, Warning, TEXT("Unknown render.view_source '%s' — using primary"), *Value);
	return EViewSource::Primary;
}
```

In the `operational` YAML block, after `health_http_port`:

```cpp
			YamlBool  (OpNode, "snapshot_endpoint_enabled", Cfg.Operational.bSnapshotEndpointEnabled);
			YamlString(OpNode, "frame_stats_path",          Cfg.Operational.FrameStatsPath);
```

After the `operational` block:

```cpp
		// ROADMAP 3A: render path
		if (YamlHas(Root, "render"))
		{
			ryml::ConstNodeRef RNode = Root["render"];
			YamlString(RNode, "view_source",             Cfg.Render.ViewSource);
			YamlFloat (RNode, "camera_cut_distance_m",   Cfg.Render.CameraCutDistanceM);
			YamlFloat (RNode, "camera_cut_angle_deg",    Cfg.Render.CameraCutAngleDeg);
			YamlDouble(RNode, "origin_shift_distance_m", Cfg.Render.OriginShiftDistanceM);
			Cfg.Render.ViewSourceMode = ParseViewSource(Cfg.Render.ViewSource);
		}
```

After the Phase 28 operational env overrides:

```cpp
	Cfg.Operational.bSnapshotEndpointEnabled = GetEnvBool(TEXT("CAMSIM_SNAPSHOT_ENDPOINT_ENABLED"), Cfg.Operational.bSnapshotEndpointEnabled);
	Cfg.Operational.FrameStatsPath           = GetEnv    (TEXT("CAMSIM_FRAME_STATS_PATH"),          Cfg.Operational.FrameStatsPath);

	// ROADMAP 3A: render path env overrides
	Cfg.Render.ViewSource           = GetEnv      (TEXT("CAMSIM_RENDER_VIEW_SOURCE"),             Cfg.Render.ViewSource);
	Cfg.Render.ViewSourceMode       = ParseViewSource(Cfg.Render.ViewSource);
	Cfg.Render.CameraCutDistanceM   = GetEnvFloat (TEXT("CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M"),   Cfg.Render.CameraCutDistanceM);
	Cfg.Render.CameraCutAngleDeg    = GetEnvFloat (TEXT("CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG"),    Cfg.Render.CameraCutAngleDeg);
	Cfg.Render.OriginShiftDistanceM = GetEnvDouble(TEXT("CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M"), Cfg.Render.OriginShiftDistanceM);
```

With the variable unset, `ParseViewSource` is called on the YAML value again. That is harmless, but it would log the typo warning twice. That's why the bad-value case in the test expects the message exactly once from `LoadFromYamlString`. Check that `LoadFromYamlString` doesn't also call `ApplyEnvOverrides` (`grep -n "ApplyEnvOverrides" Config/CamSimConfig.cpp`). If it does, change the test's expected count to 2.

- [ ] **Step 5: Add the keys to the canonical config and docs**

In `deploy/camsim_config.yaml`, under the existing `operational:` section:

```yaml
  # 3A: GET /snapshot on the health port returns the next grabbed frame as PNG
  # (lossless, before sensor effects). For the bench harness; off in production.
  # Env: CAMSIM_SNAPSHOT_ENDPOINT_ENABLED
  snapshot_endpoint_enabled: false
  # 3A: per-frame JSONL render stats (frame time, thread/GPU time, tiles, SSE).
  # Empty = disabled. Env: CAMSIM_FRAME_STATS_PATH
  frame_stats_path: ""
```

As a new top-level section after `operational:`:

```yaml
# --- Render path (ROADMAP 3A) ---
render:
  # primary: the sensor is the game viewport's view (TSR, one scene render per frame).
  # scene_capture: legacy SceneCapture2D path, kept for A/B comparison until 3B.
  # Env: CAMSIM_RENDER_VIEW_SOURCE
  view_source: primary
  # A pose jump above either threshold in one frame resets TSR history (camera cut).
  # Env: CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M / CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG
  camera_cut_distance_m: 500.0
  camera_cut_angle_deg: 30.0
  # Rebase the Cesium georeference when the camera is this far (m) from the
  # origin, keeping "up" = +Z and coordinates small. 0 = disabled.
  # Env: CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M
  origin_shift_distance_m: 0.0
```

In `docs/configuration.md`, add rows for all six keys in the same table format the file already uses (`grep -n "health_http_port" docs/configuration.md` to find the operational table, and add a `render` section after it).

- [ ] **Step 6: Build and run the tests**

Run: `scripts/run.sh --build-only`, then the headless command with `RunTests CamSim.Config`.
Expected: `CamSim.Config.RenderSection`, `CamSim.Config.RenderEnvOverrides` and `CamSim.Config.CanonicalConfigHasNoUnknownKeys` all pass.

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/RenderConfigTest.cpp \
        deploy/camsim_config.yaml docs/configuration.md
git commit -m "feat(config): render path, snapshot and frame-stats keys (3A)"
```

---

### Task 2: Frame-stats recorder and view-family counter

**Files:**
- Create: `Camera/CamSimFrameStats.h`, `Camera/CamSimFrameStats.cpp`
- Modify: `Camera/CamSimCamera.h`, `Camera/CamSimCamera.cpp` (own and feed the recorder)
- Test: `Tests/RenderPathTest.cpp` (create)

**Interfaces:**
- Consumes: `FCamSimConfig::Operational.FrameStatsPath` (Task 1).
- Produces:
  - `struct FCamSimFrameStatsSample { double UtcSeconds; double WallMs; double GameMs; double RenderMs; double RhiMs; double GpuMs; uint64 FramesEmitted; uint64 FramesDropped; float MinLoadProgressPct; double Sse; bool bCameraCut; int32 ViewFamilies; }`
  - `FString CamSimFormatFrameStatsRow(const FCamSimFrameStatsSample& S)`: one JSON object, no trailing newline, keys `t, wall_ms, game_ms, render_ms, rhi_ms, gpu_ms, emitted, dropped, load_pct, sse, cut, families`.
  - `class FCamSimFrameStatsRecorder { bool Open(const FString& Path); void Record(const FCamSimFrameStatsSample& S); void Close(); bool IsOpen() const; }`
  - `class FCamSimViewFamilyCounter : public FSceneViewExtensionBase { int32 ConsumeCount(); }`: counts every scene view family rendered (game viewport and scene captures).
  - `ACamSimCamera::bCameraCutThisFrame` (bool member, reset each tick). Task 8 sets it.

- [ ] **Step 1: Write the failing tests**

Create `Tests/RenderPathTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Camera/CamSimFrameStats.h"

// -------------------------------------------------------------------------
// ROADMAP 3A: frame-stats rows are valid JSON with the keys the bench
// harness reads, and a bad path turns stats off instead of failing.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameStatsRowTest,
	"CamSim.Render.FrameStats.RowFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameStatsRowTest::RunTest(const FString& Parameters)
{
	FCamSimFrameStatsSample S;
	S.UtcSeconds = 1790000000.25;
	S.WallMs = 33.5; S.GameMs = 4.0; S.RenderMs = 8.0; S.RhiMs = 2.0; S.GpuMs = 21.0;
	S.FramesEmitted = 42; S.FramesDropped = 1;
	S.MinLoadProgressPct = 87.5f; S.Sse = 16.0; S.bCameraCut = true; S.ViewFamilies = 2;

	const FString Row = CamSimFormatFrameStatsRow(S);
	TestFalse(TEXT("no newline"), Row.Contains(TEXT("\n")));

	TSharedPtr<FJsonObject> Obj;
	if (!TestTrue(TEXT("valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Row), Obj) && Obj.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("t"), Obj->GetNumberField(TEXT("t")), 1790000000.25);
	TestEqual(TEXT("wall_ms"), Obj->GetNumberField(TEXT("wall_ms")), 33.5);
	TestEqual(TEXT("gpu_ms"), Obj->GetNumberField(TEXT("gpu_ms")), 21.0);
	TestEqual(TEXT("emitted"), static_cast<int32>(Obj->GetNumberField(TEXT("emitted"))), 42);
	TestEqual(TEXT("dropped"), static_cast<int32>(Obj->GetNumberField(TEXT("dropped"))), 1);
	TestEqual(TEXT("load_pct"), Obj->GetNumberField(TEXT("load_pct")), 87.5);
	TestEqual(TEXT("sse"), Obj->GetNumberField(TEXT("sse")), 16.0);
	TestTrue(TEXT("cut"), Obj->GetBoolField(TEXT("cut")));
	TestEqual(TEXT("families"), static_cast<int32>(Obj->GetNumberField(TEXT("families"))), 2);
	for (const TCHAR* Key : { TEXT("game_ms"), TEXT("render_ms"), TEXT("rhi_ms") })
	{
		TestTrue(FString::Printf(TEXT("has %s"), Key), Obj->HasField(Key));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameStatsRecorderTest,
	"CamSim.Render.FrameStats.RecorderWritesLines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameStatsRecorderTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests"), TEXT("frame_stats_test.jsonl"));
	IFileManager::Get().Delete(*Path);

	FCamSimFrameStatsRecorder Recorder;
	TestTrue(TEXT("opens"), Recorder.Open(Path));
	FCamSimFrameStatsSample S;
	for (int32 I = 0; I < 3; ++I) { S.FramesEmitted = I; Recorder.Record(S); }
	Recorder.Close();
	TestFalse(TEXT("closed"), Recorder.IsOpen());

	TArray<FString> Lines;
	TestTrue(TEXT("readable"), FFileHelper::LoadFileToStringArray(Lines, *Path));
	TestEqual(TEXT("three rows"), Lines.Num(), 3);
	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameStatsOpenFailureTest,
	"CamSim.Render.FrameStats.OpenFailureIsSoft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameStatsOpenFailureTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("frame stats disabled"), EAutomationExpectedErrorFlags::Contains, 1);
	FCamSimFrameStatsRecorder Recorder;
	TestFalse(TEXT("unwritable path fails"), Recorder.Open(TEXT("/nonexistent-root-dir/camsim/frames.jsonl")));
	FCamSimFrameStatsSample S;
	Recorder.Record(S);  // must be a silent no-op
	TestFalse(TEXT("still closed"), Recorder.IsOpen());
	return true;
}
```

`Open` creates missing parent directories under writable locations. `/nonexistent-root-dir` fails on macOS and Linux because non-root users can't create directories in `/`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `scripts/run.sh --build-only`
Expected: compile error, `'Camera/CamSimFrameStats.h' file not found`.

- [ ] **Step 3: Write the header**

Create `Camera/CamSimFrameStats.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class IFileHandle;

/** One game-thread tick of render stats for the bench harness (ROADMAP 3A). */
struct FCamSimFrameStatsSample
{
	double UtcSeconds         = 0.0;  // Unix time, same clock as Python time.time()
	double WallMs             = 0.0;  // wall-clock time since the previous tick
	double GameMs             = 0.0;  // `stat unit` Game
	double RenderMs           = 0.0;  // `stat unit` Draw
	double RhiMs              = 0.0;  // `stat unit` RHIT
	double GpuMs              = 0.0;  // `stat unit` GPU
	uint64 FramesEmitted      = 0;    // frames handed to the sensor model so far
	uint64 FramesDropped      = 0;    // frames the encoder queue dropped so far
	float  MinLoadProgressPct = 100.0f;
	double Sse                = 0.0;  // current Cesium maximum screen-space error
	bool   bCameraCut         = false;
	int32  ViewFamilies       = 0;    // scene renders this frame (viewport + captures)
};

/** One JSON object, no trailing newline. Keys are the harness's contract (scripts/bench/analyze.py). */
FString CamSimFormatFrameStatsRow(const FCamSimFrameStatsSample& S);

/** Appends one JSON row per tick. Game thread only. A failed Open() leaves it off. */
class FCamSimFrameStatsRecorder
{
public:
	~FCamSimFrameStatsRecorder() { Close(); }

	bool Open(const FString& Path);
	void Record(const FCamSimFrameStatsSample& S);
	void Close();
	bool IsOpen() const { return Handle != nullptr; }

private:
	IFileHandle* Handle = nullptr;
	int32 RowsSinceFlush = 0;
};

/**
 * Counts scene view families as they begin rendering: the game viewport and
 * every SceneCapture. The harness uses it to prove one render per frame.
 */
class FCamSimViewFamilyCounter : public FSceneViewExtensionBase
{
public:
	explicit FCamSimViewFamilyCounter(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override
	{
		Count.Store(Count.Load(EMemoryOrder::Relaxed) + 1, EMemoryOrder::Relaxed);
	}

	/** Families counted since the last call. Game thread. */
	int32 ConsumeCount() { return Count.Exchange(0); }

private:
	TAtomic<int32> Count { 0 };
};
```

- [ ] **Step 4: Write the implementation**

Create `Camera/CamSimFrameStats.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimFrameStats.h"
#include "CamSimTest.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

FString CamSimFormatFrameStatsRow(const FCamSimFrameStatsSample& S)
{
	return FString::Printf(
		TEXT("{\"t\":%.6f,\"wall_ms\":%.3f,\"game_ms\":%.3f,\"render_ms\":%.3f,\"rhi_ms\":%.3f,\"gpu_ms\":%.3f,")
		TEXT("\"emitted\":%llu,\"dropped\":%llu,\"load_pct\":%.2f,\"sse\":%.3f,\"cut\":%s,\"families\":%d}"),
		S.UtcSeconds, S.WallMs, S.GameMs, S.RenderMs, S.RhiMs, S.GpuMs,
		static_cast<unsigned long long>(S.FramesEmitted), static_cast<unsigned long long>(S.FramesDropped),
		S.MinLoadProgressPct, S.Sse, S.bCameraCut ? TEXT("true") : TEXT("false"), S.ViewFamilies);
}

bool FCamSimFrameStatsRecorder::Open(const FString& Path)
{
	Close();
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*FPaths::GetPath(Path));
	Handle = PF.OpenWrite(*Path, /*bAppend=*/false);
	if (!Handle)
	{
		UE_LOG(LogCamSim, Warning, TEXT("FrameStats: cannot open '%s' — frame stats disabled"), *Path);
		return false;
	}
	UE_LOG(LogCamSim, Log, TEXT("FrameStats: writing %s"), *Path);
	return true;
}

void FCamSimFrameStatsRecorder::Record(const FCamSimFrameStatsSample& S)
{
	if (!Handle) return;
	FTCHARToUTF8 Utf8(*(CamSimFormatFrameStatsRow(S) + TEXT("\n")));
	Handle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	// Flush about once a second so a crashed run still leaves usable data.
	if (++RowsSinceFlush >= 30)
	{
		Handle->Flush();
		RowsSinceFlush = 0;
	}
}

void FCamSimFrameStatsRecorder::Close()
{
	if (!Handle) return;
	Handle->Flush();
	delete Handle;
	Handle = nullptr;
}
```

- [ ] **Step 5: Wire the recorder into the camera**

In `Camera/CamSimCamera.h`:
- add `#include "Camera/CamSimFrameStats.h"` next to the other `Camera/` includes;
- add these private members after `FCamSimStreamingController Streaming;`:

```cpp
	/** ROADMAP 3A bench stats (off unless operational.frame_stats_path is set). */
	FCamSimFrameStatsRecorder FrameStats;
	TSharedPtr<FCamSimViewFamilyCounter, ESPMode::ThreadSafe> ViewFamilyCounter;
	double LastStatsWallSec = 0.0;
	/** Set by the camera-cut check (Task 8); recorded in frame stats. */
	bool bCameraCutThisFrame = false;

	void RecordFrameStats();
```

In `Camera/CamSimCamera.cpp`, add the includes:

```cpp
#include "RenderTimer.h"      // GGameThreadTime, GRenderThreadTime, GRHIThreadTime
#include "DynamicRHI.h"       // RHIGetGPUFrameCycles
#include "Cesium3DTileset.h"
```

At the end of `BeginPlay()`, before the "ready" log:

```cpp
	if (!Cfg.Operational.FrameStatsPath.IsEmpty() && FrameStats.Open(Cfg.Operational.FrameStatsPath))
	{
		ViewFamilyCounter = FSceneViewExtensions::NewExtension<FCamSimViewFamilyCounter>();
		LastStatsWallSec = FPlatformTime::Seconds();
	}
```

In `EndPlay()`, before `Super::EndPlay`:

```cpp
	FrameStats.Close();
	ViewFamilyCounter.Reset();
```

At the start of `Tick()`, after `if (!Subsystem) return;`:

```cpp
	RecordFrameStats();  // stats for the frame that just finished
	bCameraCutThisFrame = false;
```

New function:

```cpp
void ACamSimCamera::RecordFrameStats()
{
	if (!FrameStats.IsOpen()) return;

	const double NowSec = FPlatformTime::Seconds();
	const double MsPerCycle = FPlatformTime::GetSecondsPerCycle() * 1000.0;

	FCamSimFrameStatsSample S;
	S.UtcSeconds    = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalSeconds();
	S.WallMs        = (NowSec - LastStatsWallSec) * 1000.0;
	S.GameMs        = GGameThreadTime   * MsPerCycle;
	S.RenderMs      = GRenderThreadTime * MsPerCycle;
	S.RhiMs         = GRHIThreadTime    * MsPerCycle;
	S.GpuMs         = RHIGetGPUFrameCycles(0) * MsPerCycle;
	S.FramesEmitted = CaptureComp->GetFramesCaptured();
	S.FramesDropped = CaptureComp->GetDroppedFrameCount();
	S.bCameraCut    = bCameraCutThisFrame;
	S.ViewFamilies  = ViewFamilyCounter ? ViewFamilyCounter->ConsumeCount() : 0;

	float MinLoad = 100.0f;
	double Sse = 0.0;
	for (const TWeakObjectPtr<ACesium3DTileset>& Weak : Subsystem->GetCachedTilesets())
	{
		if (const ACesium3DTileset* T = Weak.Get())
		{
			MinLoad = FMath::Min(MinLoad, T->GetLoadProgress());  // already 0-100
			Sse = T->MaximumScreenSpaceError;
		}
	}
	S.MinLoadProgressPct = MinLoad;
	S.Sse = Sse;

	FrameStats.Record(S);
	LastStatsWallSec = NowSec;
}
```

If `ACesium3DTileset::MaximumScreenSpaceError` isn't publicly readable, use its getter: `grep -n "ScreenSpaceError" "/Users/Shared/Epic Games/UE_5.8/Engine/Plugins/Marketplace/CesiumForUnreal/Source/CesiumRuntime/Public/Cesium3DTileset.h"`. Match what `FCamSimStreamingController::SetScreenSpaceError` uses.

- [ ] **Step 6: Build and run the tests**

Run: `scripts/run.sh --build-only`, then headless `RunTests CamSim.Render`.
Expected: the three `CamSim.Render.FrameStats.*` tests pass.

- [ ] **Step 7: Check a real run writes rows**

```bash
CAMSIM_FRAME_STATS_PATH="$PWD/.cache/stats-check.jsonl" scripts/run.sh --headless --local --detach
python3 scripts/send_cigi_test.py --duration 60 --lat 37.7749 --lon -122.4194 --alt 3000 &
sleep 90; scripts/stop.sh
tail -3 .cache/stats-check.jsonl
```

Expected: rows with non-zero `gpu_ms` and `wall_ms` near 33. **Record the `families` value.** If it is 2 on the current path, the double render is confirmed. Put the value in the commit message.

- [ ] **Step 8: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameStats.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameStats.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/RenderPathTest.cpp
git commit -m "feat(camera): per-frame render stats for the bench harness (3A)"
```

---

### Task 3: Snapshot service and `GET /snapshot`

**Files:**
- Create: `Health/CamSimSnapshotService.h`, `Health/CamSimSnapshotService.cpp`
- Modify: `Health/CamSimHealthServer.h/.cpp` (add `BindSnapshotRoute`)
- Modify: `Subsystem/CamSimSubsystem.h/.cpp` (own the service, bind the route, tick timeouts)
- Modify: `Camera/CamSimCaptureComponent.cpp` (offer each completed readback)
- Test: `Tests/SnapshotEndpointTest.cpp` (create)

**Interfaces:**
- Consumes: `FCamSimConfig::Operational.bSnapshotEndpointEnabled` (Task 1).
- Produces:
  - `class FCamSimSnapshotService` with:
    - `void Request(FHttpResultCallback OnComplete)` (game thread);
    - `bool WantsFrame() const`;
    - `void OfferFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height)` (game thread);
    - `void Tick(double NowSec)`, which answers 503 for requests older than `TimeoutSec`;
    - `double TimeoutSec = 5.0`.
  - `void FCamSimHealthServer::BindSnapshotRoute(TFunction<void(FHttpResultCallback)> Handler)`.
  - `FCamSimSnapshotService* UCamSimSubsystem::GetSnapshotService() const` (null when disabled).

- [ ] **Step 1: Write the failing tests**

Create `Tests/SnapshotEndpointTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Health/CamSimHealthServer.h"
#include "Health/CamSimSnapshotService.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IHttpRequest.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

// -------------------------------------------------------------------------
// ROADMAP 3A: GET /snapshot. Unbound route is 404; a request with no frame
// times out as 503; an offered frame comes back as a PNG of the same size.
// Both tickers are pumped (see CLAUDE.md "HTTP automation tests").
// -------------------------------------------------------------------------

namespace
{
	struct FGetResult { bool bCompleted = false; int32 Code = 0; TArray<uint8> Body; FString ContentType; };

	FGetResult HttpGet(int32 Port, const TCHAR* Path, TFunction<void()> PerTick = nullptr, double TimeoutSec = 8.0)
	{
		FGetResult R;
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
		Req->SetVerb(TEXT("GET"));
		Req->SetURL(FString::Printf(TEXT("http://127.0.0.1:%d%s"), Port, Path));
		Req->SetTimeout(TimeoutSec);
		Req->OnProcessRequestComplete().BindLambda([&R](FHttpRequestPtr, FHttpResponsePtr Resp, bool)
		{
			R.bCompleted = true;
			if (Resp.IsValid())
			{
				R.Code = Resp->GetResponseCode();
				R.Body = Resp->GetContent();
				R.ContentType = Resp->GetContentType();
			}
		});
		Req->ProcessRequest();
		const double Deadline = FPlatformTime::Seconds() + TimeoutSec;
		while (!R.bCompleted && FPlatformTime::Seconds() < Deadline)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.01f);
			FTSTicker::GetCoreTicker().Tick(0.01f);
			if (PerTick) PerTick();
			FPlatformProcess::Sleep(0.01f);
		}
		return R;
	}

	bool StartServer(FCamSimHealthServer& Server, int32 Port)
	{
		auto True = [](){ return true; };
		return Server.Start(Port, True, True, True, True, True, []() -> FString { return TEXT(""); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotUnboundTest,
	"CamSim.Health.Snapshot.UnboundIs404",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSnapshotUnboundTest::RunTest(const FString& Parameters)
{
	constexpr int32 Port = 48081;
	FCamSimHealthServer Server;
	if (!TestTrue(TEXT("started"), StartServer(Server, Port))) return false;
	const FGetResult R = HttpGet(Port, TEXT("/snapshot"));
	Server.Stop();
	TestTrue(TEXT("completed"), R.bCompleted);
	TestEqual(TEXT("404 when not enabled"), R.Code, 404);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotTimeoutTest,
	"CamSim.Health.Snapshot.TimesOutWithoutFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSnapshotTimeoutTest::RunTest(const FString& Parameters)
{
	constexpr int32 Port = 48082;
	FCamSimSnapshotService Service;
	Service.TimeoutSec = 0.5;
	FCamSimHealthServer Server;
	if (!TestTrue(TEXT("started"), StartServer(Server, Port))) return false;
	Server.BindSnapshotRoute([&Service](FHttpResultCallback OnComplete) { Service.Request(MoveTemp(OnComplete)); });

	const FGetResult R = HttpGet(Port, TEXT("/snapshot"), [&Service]() { Service.Tick(FPlatformTime::Seconds()); });
	Server.Stop();
	TestTrue(TEXT("completed"), R.bCompleted);
	TestEqual(TEXT("503 when no frame arrives"), R.Code, 503);
	TestFalse(TEXT("no longer waiting"), Service.WantsFrame());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotPngTest,
	"CamSim.Health.Snapshot.ReturnsPng",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSnapshotPngTest::RunTest(const FString& Parameters)
{
	constexpr int32 Port = 48083;
	constexpr int32 W = 64, H = 36;
	FCamSimSnapshotService Service;
	FCamSimHealthServer Server;
	if (!TestTrue(TEXT("started"), StartServer(Server, Port))) return false;
	Server.BindSnapshotRoute([&Service](FHttpResultCallback OnComplete) { Service.Request(MoveTemp(OnComplete)); });

	TArray<FColor> Pixels;
	Pixels.Init(FColor(10, 200, 30, 255), W * H);
	const FGetResult R = HttpGet(Port, TEXT("/snapshot"), [&]()
	{
		if (Service.WantsFrame()) Service.OfferFrame(Pixels, W, H);
		Service.Tick(FPlatformTime::Seconds());
	});
	Server.Stop();

	TestEqual(TEXT("200"), R.Code, 200);
	TestTrue(TEXT("image/png"), R.ContentType.Contains(TEXT("image/png")));

	IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Png = IWM.CreateImageWrapper(EImageFormat::PNG);
	if (!TestTrue(TEXT("decodes"), Png.IsValid() && Png->SetCompressed(R.Body.GetData(), R.Body.Num()))) return false;
	TestEqual(TEXT("width"), static_cast<int32>(Png->GetWidth()), W);
	TestEqual(TEXT("height"), static_cast<int32>(Png->GetHeight()), H);
	TArray64<uint8> Raw;
	TestTrue(TEXT("raw"), Png->GetRaw(ERGBFormat::BGRA, 8, Raw));
	TestEqual(TEXT("green channel survives"), static_cast<int32>(Raw[1]), 200);
	return true;
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `scripts/run.sh --build-only`
Expected: compile error, `'Health/CamSimSnapshotService.h' file not found`.

- [ ] **Step 3: Write the service**

Create `Health/CamSimSnapshotService.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"

/**
 * FCamSimSnapshotService
 *
 * Answers GET /snapshot with the next grabbed frame as PNG (ROADMAP 3A). The
 * frame is the render before sensor effects and encoding, so the bench's
 * reference shots are lossless. HTTP handlers run on the game thread (the
 * listener is pumped by the core ticker), so Request/OfferFrame/Tick are
 * game-thread calls. PNG encoding runs on a background task; the response is
 * completed back on the game thread.
 */
class FCamSimSnapshotService
{
public:
	~FCamSimSnapshotService();

	/** Queue a request; answered by the next OfferFrame() or by Tick() after TimeoutSec. */
	void Request(FHttpResultCallback OnComplete);

	/** True while at least one request is waiting for a frame. */
	bool WantsFrame() const { return Pending.Num() > 0; }

	/** Hand over a finished frame; copies only when requests are waiting. */
	void OfferFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height);

	/** Answer 503 to requests older than TimeoutSec. */
	void Tick(double NowSec);

	double TimeoutSec = 5.0;

private:
	struct FPendingRequest
	{
		FHttpResultCallback OnComplete;
		double RequestedAtSec = 0.0;
	};
	TArray<FPendingRequest> Pending;

	/** Cleared in the destructor so a late PNG task doesn't answer into a dead service. */
	TSharedRef<TAtomic<bool>, ESPMode::ThreadSafe> bAlive = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(true);
};
```

Create `Health/CamSimSnapshotService.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Health/CamSimSnapshotService.h"
#include "CamSimTest.h"
#include "HttpServerResponse.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Async/Async.h"

FCamSimSnapshotService::~FCamSimSnapshotService()
{
	bAlive->Store(false);
}

void FCamSimSnapshotService::Request(FHttpResultCallback OnComplete)
{
	checkSlow(IsInGameThread());
	Pending.Add({ MoveTemp(OnComplete), FPlatformTime::Seconds() });
}

void FCamSimSnapshotService::OfferFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	checkSlow(IsInGameThread());
	if (Pending.Num() == 0 || Pixels.Num() != Width * Height || Width <= 0) return;

	TArray<FPendingRequest> Waiting = MoveTemp(Pending);
	Pending.Reset();

	TSharedRef<TAtomic<bool>, ESPMode::ThreadSafe> Alive = bAlive;
	Async(EAsyncExecution::ThreadPool,
		[Alive, Waiting = MoveTemp(Waiting), Pixels = TArray<FColor>(Pixels), Width, Height]() mutable
	{
		IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Png = IWM.CreateImageWrapper(EImageFormat::PNG);
		TArray64<uint8> Encoded;
		if (Png.IsValid() && Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			Encoded = Png->GetCompressed(0);
		}

		AsyncTask(ENamedThreads::GameThread, [Alive, Waiting = MoveTemp(Waiting), Encoded = MoveTemp(Encoded)]() mutable
		{
			if (!Alive->Load()) return;
			for (FPendingRequest& Req : Waiting)
			{
				TUniquePtr<FHttpServerResponse> Response;
				if (Encoded.Num() > 0)
				{
					Response = FHttpServerResponse::Create(
						TArray<uint8>(Encoded.GetData(), static_cast<int32>(Encoded.Num())), TEXT("image/png"));
				}
				else
				{
					Response = FHttpServerResponse::Error(EHttpServerResponseCodes::ServerError,
						TEXT("encode_failed"), TEXT("PNG encode failed"));
				}
				Req.OnComplete(MoveTemp(Response));
			}
		});
	});
}

void FCamSimSnapshotService::Tick(double NowSec)
{
	for (int32 I = Pending.Num() - 1; I >= 0; --I)
	{
		if (NowSec - Pending[I].RequestedAtSec < TimeoutSec) continue;
		auto Response = FHttpServerResponse::Create(
			FString(TEXT("{\"status\":\"no_frame\"}")), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::ServiceUnavail;
		Pending[I].OnComplete(MoveTemp(Response));
		Pending.RemoveAt(I);
	}
}
```

Before relying on `FHttpServerResponse::Create(TArray<uint8>, contentType)` and `FHttpServerResponse::Error(...)`, check their exact signatures: `grep -n "static.*Create\|static.*Error" "/Users/Shared/Epic Games/UE_5.8/Engine/Source/Runtime/Online/HTTPServer/Public/HttpServerResponse.h"`. Use the overloads that exist. If `Error` needs different arguments, build the 500 response the same way `Tick` builds the 503.

- [ ] **Step 4: Add the route**

In `Health/CamSimHealthServer.h`, add a public method and update the routes comment:

```cpp
	/**
	 * Bind GET /snapshot (ROADMAP 3A). Unbound, the router answers 404. The
	 * handler owns the callback and must complete it exactly once.
	 */
	void BindSnapshotRoute(TFunction<void(FHttpResultCallback)> Handler);
```

Add `#include "HttpResultCallback.h"` to the header.

In `Health/CamSimHealthServer.cpp`:

```cpp
void FCamSimHealthServer::BindSnapshotRoute(TFunction<void(FHttpResultCallback)> Handler)
{
	if (!Router) return;
	Router->BindRoute(FHttpPath(TEXT("/snapshot")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([Handler = MoveTemp(Handler)](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			Handler(OnComplete);
			return true;
		}));
	UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: /snapshot enabled on port %d"), ListenPort);
}
```

- [ ] **Step 5: Own the service in the subsystem and feed it frames**

In `Subsystem/CamSimSubsystem.cpp`, inside `FSubsystemImpl`, next to `HealthServer`:

```cpp
	TUniquePtr<FCamSimSnapshotService>  SnapshotService;
```

Include `Health/CamSimSnapshotService.h`. Right after `Impl->HealthServer->Start(...)` succeeds:

```cpp
		if (Config.Operational.bSnapshotEndpointEnabled)
		{
			Impl->SnapshotService = MakeUnique<FCamSimSnapshotService>();
			FCamSimSnapshotService* Snap = Impl->SnapshotService.Get();
			Impl->HealthServer->BindSnapshotRoute([Snap](FHttpResultCallback OnComplete) { Snap->Request(MoveTemp(OnComplete)); });
		}
```

In the teardown that resets `HealthServer` (around line 106), reset `SnapshotService` **after** stopping the server. In `UCamSimSubsystem::Tick`, next to `Impl->HealthServer->UpdateTick()`:

```cpp
		if (Impl->SnapshotService) { Impl->SnapshotService->Tick(FPlatformTime::Seconds()); }
```

Add a public accessor in `Subsystem/CamSimSubsystem.h` (forward-declare `class FCamSimSnapshotService;`):

```cpp
	/** GET /snapshot service (ROADMAP 3A); null unless operational.snapshot_endpoint_enabled. */
	FCamSimSnapshotService* GetSnapshotService() const;
```

Implement it in the `.cpp` as `return Impl ? Impl->SnapshotService.Get() : nullptr;`.

In `Camera/CamSimCaptureComponent.cpp` `Poll()`, in the `State == EReadbackState::Complete` branch, right after `TArray<FColor> Pixels = MoveTemp(AsyncPixels);`:

```cpp
		// ROADMAP 3A reference shots: the pre-sensor frame, lossless.
		if (FCamSimSnapshotService* Snap = Subsystem ? Subsystem->GetSnapshotService() : nullptr)
		{
			if (Snap->WantsFrame())
			{
				const FCamSimConfig& SnapCfg = Subsystem->GetConfig();
				Snap->OfferFrame(Pixels, SnapCfg.CaptureWidth, SnapCfg.CaptureHeight);
			}
		}
```

Include `Health/CamSimSnapshotService.h` there.

- [ ] **Step 6: Build and run the tests**

Run: `scripts/run.sh --build-only`, then headless `RunTests CamSim.Health+CamSim.HttpServer`. If the `+` separator isn't accepted there, run the two filters separately.
Expected: the three `CamSim.Health.Snapshot.*` tests and the existing `CamSim.HttpServer.*` tests pass.

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Health \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCaptureComponent.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/SnapshotEndpointTest.cpp
git commit -m "feat(health): GET /snapshot returns the pre-sensor frame as PNG (3A)"
```

---

### Task 4: Bench harness (scenario, runner, analysis, compare)

**Files:**
- Modify: `scripts/send_cigi_test.py` (add `pack_art_part_control`)
- Create: `scripts/bench/__init__.py` (empty), `scripts/bench/scenario.py`, `scripts/bench/analyze.py`, `scripts/bench/compare.py`, `scripts/bench/run_bench.py`, `scripts/bench/README.md`
- Test: `scripts/tests/test_bench_scenario.py`, `scripts/tests/test_bench_analyze.py`, `scripts/tests/test_bench_compare.py`

**Interfaces:**
- Consumes: CamSim env vars `CAMSIM_FRAME_STATS_PATH`, `CAMSIM_SNAPSHOT_ENDPOINT_ENABLED`, `CAMSIM_RENDER_VIEW_SOURCE` (Task 1); frame-stats row keys (Task 2); `GET /snapshot` (Task 3); `GET /ready` JSON field `terrain_ready`.
- Produces:
  - `scenario.Pose` (dataclass with `lat, lon, alt, yaw, pitch, roll, gimbal_yaw, gimbal_pitch, fov_h, utc_hour, utc_minute, month, day`).
  - `scenario.Phase(name: str, duration_s: float, pose_at: Callable[[float], Pose], measured: bool)`.
  - `scenario.Shot(name: str, pose: Pose)`.
  - `scenario.build_phases(smoke: bool = False) -> list[Phase]`, `scenario.build_shots(smoke: bool = False) -> list[Shot]`.
  - `scenario.host_datagram(frame_ctr: int, pose: Pose, entity_id: int = 1) -> bytes`.
  - `analyze.summarize(rows: list[dict], phases: list[dict]) -> dict`: `phases` items are `{"name", "start", "end", "measured"}`. The result maps phase name to metrics `frames, wall_ms_p50, wall_ms_p95, wall_ms_p99, game_ms_p50, render_ms_p50, rhi_ms_p50, gpu_ms_p50, gpu_ms_p95, hitches_66, hitches_100, popin_fraction, families_mean, dropped`.
  - `compare.render_table(baseline: dict, current: dict) -> str` (markdown).
  - `compare.ssim(a: numpy.ndarray, b: numpy.ndarray) -> float`.
  - CLI: `run_bench.py --label NAME [--view-source primary|scene_capture] [--smoke] [--trace] [--skip-warmup] [--out DIR]` writes `DIR/results.json`, `DIR/frames.jsonl`, `DIR/phases.json`, `DIR/shots/*.png`, `DIR/slew.ts`.

- [ ] **Step 1: Write the failing tests**

`scripts/tests/test_bench_scenario.py`:

```python
"""The bench scenario is deterministic and every phase produces valid CIGI datagrams."""
import struct

from bench import scenario


def test_phases_have_expected_names_and_are_deterministic():
    names = [p.name for p in scenario.build_phases()]
    assert names == ["warmup", "orbit", "slew", "low_pass", "far_origin"]
    a = [p.pose_at(t) for p in scenario.build_phases() for t in (0.0, 1.5, 7.25)]
    b = [p.pose_at(t) for p in scenario.build_phases() for t in (0.0, 1.5, 7.25)]
    assert a == b


def test_only_warmup_is_unmeasured():
    measured = {p.name: p.measured for p in scenario.build_phases()}
    assert measured == {"warmup": False, "orbit": True, "slew": True, "low_pass": True, "far_origin": True}


def test_smoke_is_short_and_has_one_shot():
    phases = scenario.build_phases(smoke=True)
    assert sum(p.duration_s for p in phases) <= 25
    assert len(scenario.build_shots(smoke=True)) == 1


def test_slew_phase_reaches_fast_gimbal_rates():
    slew = next(p for p in scenario.build_phases() if p.name == "slew")
    dt = 1.0 / 30.0
    rates = [abs(slew.pose_at(t + dt).gimbal_yaw - slew.pose_at(t).gimbal_yaw) / dt
             for t in [i * dt for i in range(int(slew.duration_s * 30) - 1)]]
    assert max(rates) >= 40.0  # deg/s: well above the 10 deg/s prefetch threshold


def test_far_origin_is_about_300_km_from_orbit():
    orbit = next(p for p in scenario.build_phases() if p.name == "orbit").pose_at(0.0)
    far = next(p for p in scenario.build_phases() if p.name == "far_origin").pose_at(0.0)
    # 1 deg longitude at ~37.8 N is about 87.9 km.
    km = abs(far.lon - orbit.lon) * 87.9
    assert 280.0 <= km <= 320.0


def test_host_datagram_starts_with_ig_control_and_carries_art_part():
    pose = scenario.build_phases()[1].pose_at(0.0)
    dgram = scenario.host_datagram(7, pose)
    assert dgram[0] == 1 and dgram[1] == 24          # IG Control
    assert struct.unpack(">I", dgram[8:12])[0] == 7  # frame counter
    ids = []
    i = 0
    while i < len(dgram):
        ids.append(dgram[i])
        i += dgram[i + 1]
    assert i == len(dgram)                            # packets tile the datagram
    assert 2 in ids and 6 in ids and 9 in ids         # entity, art part, celestial


def test_shot_names_are_unique_and_filesystem_safe():
    names = [s.name for s in scenario.build_shots()]
    assert len(names) == len(set(names)) >= 6
    assert all(n.replace("_", "").isalnum() for n in names)
```

`scripts/tests/test_bench_analyze.py`:

```python
"""Per-phase metrics come out of frame-stats rows as the harness expects."""
from bench import analyze


def _row(t, wall, gpu=10.0, load=100.0, fam=1, dropped=0):
    return {"t": t, "wall_ms": wall, "game_ms": 3.0, "render_ms": 5.0, "rhi_ms": 1.0,
            "gpu_ms": gpu, "emitted": 0, "dropped": dropped, "load_pct": load,
            "sse": 16.0, "cut": False, "families": fam}


def test_rows_are_split_by_phase_window_and_unmeasured_phases_skipped():
    rows = [_row(t, 33.0) for t in range(0, 10)] + [_row(t, 40.0) for t in range(10, 20)]
    phases = [{"name": "warmup", "start": 0, "end": 10, "measured": False},
              {"name": "orbit", "start": 10, "end": 20, "measured": True}]
    out = analyze.summarize(rows, phases)
    assert list(out) == ["orbit"]
    assert out["orbit"]["frames"] == 10
    assert out["orbit"]["wall_ms_p50"] == 40.0


def test_hitches_popin_families_and_drops():
    rows = [_row(0, 33.0), _row(1, 70.0, load=90.0), _row(2, 120.0, fam=2), _row(3, 33.0, load=99.9, dropped=3)]
    out = analyze.summarize(rows, [{"name": "slew", "start": 0, "end": 4, "measured": True}])["slew"]
    assert out["hitches_66"] == 2       # 70 and 120
    assert out["hitches_100"] == 1      # 120
    assert out["popin_fraction"] == 0.5
    assert out["families_mean"] == 1.25
    assert out["dropped"] == 3          # counter delta over the phase


def test_empty_phase_reports_zero_frames_not_an_error():
    out = analyze.summarize([], [{"name": "orbit", "start": 0, "end": 1, "measured": True}])
    assert out["orbit"]["frames"] == 0
```

`scripts/tests/test_bench_compare.py`:

```python
"""compare.py renders a readable delta table and a sane SSIM."""
import numpy as np

from bench import compare


def test_table_has_every_phase_and_metric_with_deltas():
    base = {"phases": {"orbit": {"wall_ms_p99": 50.0, "gpu_ms_p50": 30.0, "hitches_66": 10}}}
    cur = {"phases": {"orbit": {"wall_ms_p99": 40.0, "gpu_ms_p50": 15.0, "hitches_66": 2}}}
    table = compare.render_table(base, cur)
    assert "| orbit |" in table
    assert "wall_ms_p99" in table and "50.0" in table and "40.0" in table
    assert "-20%" in table                       # 50 -> 40
    assert "-50%" in table                       # 30 -> 15


def test_phase_missing_from_one_side_is_shown_not_dropped():
    table = compare.render_table({"phases": {"orbit": {"wall_ms_p99": 1.0}}}, {"phases": {}})
    assert "orbit" in table and "—" in table


def test_ssim_identical_is_one_and_noise_is_lower():
    rng = np.random.default_rng(0)
    a = rng.integers(0, 255, (72, 128, 3), dtype=np.uint8)
    b = np.clip(a.astype(int) + rng.integers(-60, 60, a.shape), 0, 255).astype(np.uint8)
    assert compare.ssim(a, a) == 1.0
    assert 0.0 < compare.ssim(a, b) < 0.95
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `uv run --with pytest --with numpy --with pillow pytest scripts/tests/test_bench_scenario.py scripts/tests/test_bench_analyze.py scripts/tests/test_bench_compare.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'bench'`.

- [ ] **Step 3: Add `pack_art_part_control` to the packet library**

In `scripts/send_cigi_test.py`, after `pack_sensor_control`, add the same function that `scripts/cigi_web_ui.py:92-119` already has. Copy it verbatim, including its docstring, `struct.pack(">BBHBBxxffffff", ...)` layout and the size assertion. Add to `scripts/tests/test_send_cigi.py`:

```python
def test_pack_art_part_control_layout():
    pkt = sc.pack_art_part_control(1, 0, pitch=-30.0, yaw=45.0)
    assert len(pkt) == 32 and pkt[0] == 6 and pkt[1] == 32
    assert struct.unpack(">H", pkt[2:4])[0] == 1
    assert pkt[4] == 0 and pkt[5] == 0x71
    roll, pitch, yaw = struct.unpack(">fff", pkt[20:32])
    assert (roll, pitch, yaw) == (0.0, -30.0, 45.0)
```

- [ ] **Step 4: Write `scripts/bench/scenario.py`**

```python
"""Deterministic CIGI flight for the render benchmark (ROADMAP 3A).

Phases fly around San Francisco (varied terrain, urban, water); far_origin jumps
~300 km east to exercise Cesium origin shift and lighting. Altitudes are WGS-84
ellipsoid heights, as CIGI 3.3 defines them. Times are UTC (the sim clock is UTC).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, replace
from typing import Callable

import send_cigi_test as sc

BASE_LAT = 37.7749
BASE_LON = -122.4194
FAR_LON = BASE_LON + 3.41  # ~300 km east at 37.8 N (1 deg lon ~ 87.9 km)
CAMERA_ENTITY_ID = 1       # deploy/camsim_config.yaml camera_entity_id
GIMBAL_ART_PART_ID = 0     # camera gimbal (scripts/cigi_web_ui.py)


@dataclass(frozen=True)
class Pose:
    lat: float
    lon: float
    alt: float
    yaw: float = 0.0
    pitch: float = 0.0
    roll: float = 0.0
    gimbal_yaw: float = 0.0
    gimbal_pitch: float = -30.0
    fov_h: float = 30.0
    utc_hour: int = 19   # 12:00 PDT
    utc_minute: int = 0
    month: int = 6
    day: int = 21


@dataclass(frozen=True)
class Phase:
    name: str
    duration_s: float
    pose_at: Callable[[float], Pose]
    measured: bool = True


@dataclass(frozen=True)
class Shot:
    name: str
    pose: Pose


def _orbit(lat: float, lon: float, alt: float, radius_m: float, period_s: float) -> Callable[[float], Pose]:
    def pose_at(t: float) -> Pose:
        ang = 2.0 * math.pi * t / period_s
        dlat = (radius_m * math.cos(ang)) / 111_320.0
        dlon = (radius_m * math.sin(ang)) / (111_320.0 * math.cos(math.radians(lat)))
        heading = (math.degrees(ang) + 90.0) % 360.0      # tangent to the circle
        return Pose(lat + dlat, lon + dlon, alt, yaw=heading, gimbal_yaw=-90.0, gimbal_pitch=-35.0)
    return pose_at


def _slew(t: float) -> Pose:
    # Peak yaw rate = 90 * 2*pi/10 ~ 56.5 deg/s; pitch adds up to ~18 deg/s.
    return Pose(BASE_LAT, BASE_LON, 3000.0, yaw=0.0,
                gimbal_yaw=90.0 * math.sin(2.0 * math.pi * t / 10.0),
                gimbal_pitch=-35.0 + 20.0 * math.sin(2.0 * math.pi * t / 7.0))


def _low_pass(t: float) -> Pose:
    # 100 m/s due east at 600 m HAE from the west side of the city across the bay.
    start_lon = BASE_LON - 0.05
    dlon = (100.0 * t) / (111_320.0 * math.cos(math.radians(BASE_LAT)))
    return Pose(BASE_LAT, start_lon + dlon, 600.0, yaw=90.0, gimbal_pitch=-20.0)


def build_phases(smoke: bool = False) -> list[Phase]:
    if smoke:
        return [Phase("orbit", 20.0, _orbit(BASE_LAT, BASE_LON, 3000.0, 2000.0, 120.0))]
    orbit = _orbit(BASE_LAT, BASE_LON, 3000.0, 2000.0, 120.0)
    far = _orbit(BASE_LAT, FAR_LON, 3000.0, 2000.0, 120.0)

    def warmup(t: float) -> Pose:
        # Visit every measured area once so Cesium's disk cache is warm.
        if t < 60.0:
            return orbit(t * 2.0)
        if t < 90.0:
            return _low_pass((t - 60.0) * 3.0)
        return far(t - 90.0)

    return [
        Phase("warmup", 120.0, warmup, measured=False),
        Phase("orbit", 120.0, orbit),
        Phase("slew", 60.0, _slew),
        Phase("low_pass", 90.0, _low_pass),
        Phase("far_origin", 90.0, far),
    ]


def build_shots(smoke: bool = False) -> list[Shot]:
    nadir = Pose(BASE_LAT, BASE_LON, 3000.0, gimbal_pitch=-90.0)
    if smoke:
        return [Shot("nadir_3km", nadir)]
    slant = Pose(BASE_LAT - 0.06, BASE_LON, 3000.0, yaw=0.0, gimbal_pitch=-17.0)   # ~10 km slant
    horizon = Pose(BASE_LAT, BASE_LON, 1500.0, yaw=270.0, gimbal_pitch=-2.0)
    low_oblique = Pose(BASE_LAT, BASE_LON - 0.03, 400.0, yaw=90.0, gimbal_pitch=-12.0)
    far_slant = replace(slant, lon=FAR_LON)
    return [
        Shot("nadir_3km", nadir),
        Shot("slant_10km", slant),
        Shot("horizon", horizon),
        Shot("low_oblique", low_oblique),
        Shot("dawn_slant", replace(slant, utc_hour=13, utc_minute=15)),   # ~06:15 PDT
        Shot("dusk_slant", replace(slant, utc_hour=3, utc_minute=15, day=22)),  # ~20:15 PDT
        Shot("far_origin_slant", far_slant),
        Shot("far_origin_nadir", replace(nadir, lon=FAR_LON)),
    ]


def host_datagram(frame_ctr: int, pose: Pose, entity_id: int = CAMERA_ENTITY_ID) -> bytes:
    celestial = {"hour": pose.utc_hour, "minute": pose.utc_minute,
                 "month": pose.month, "day": pose.day, "year": 2026}
    dgram = sc.build_host_frame(frame_ctr, entity_id, pose.lat, pose.lon, pose.alt,
                                pose.yaw, pose.pitch, pose.roll, fov_h=pose.fov_h, celestial=celestial)
    return dgram + sc.pack_art_part_control(entity_id, GIMBAL_ART_PART_ID,
                                            pitch=pose.gimbal_pitch, yaw=pose.gimbal_yaw)
```

Check `pack_celestial_control`'s keyword names against `scripts/send_cigi_test.py:286-330`. If they differ from `hour, minute, month, day, year`, use the real names.

- [ ] **Step 5: Write `scripts/bench/analyze.py`**

```python
"""Turn frame-stats JSONL rows into per-phase metrics (ROADMAP 3A)."""
from __future__ import annotations

import json
from pathlib import Path

HITCH_MS = 66.7        # two frames at 30 fps
BIG_HITCH_MS = 100.0


def _pct(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    k = max(0, min(len(s) - 1, round(p / 100.0 * (len(s) - 1))))
    return float(s[k])


def load_rows(path: Path) -> list[dict]:
    rows = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            rows.append(json.loads(line))
    return rows


def summarize(rows: list[dict], phases: list[dict]) -> dict:
    out: dict[str, dict] = {}
    for ph in phases:
        if not ph["measured"]:
            continue
        sel = [r for r in rows if ph["start"] <= r["t"] < ph["end"]]
        wall = [r["wall_ms"] for r in sel]
        out[ph["name"]] = {
            "frames": len(sel),
            "wall_ms_p50": _pct(wall, 50), "wall_ms_p95": _pct(wall, 95), "wall_ms_p99": _pct(wall, 99),
            "game_ms_p50": _pct([r["game_ms"] for r in sel], 50),
            "render_ms_p50": _pct([r["render_ms"] for r in sel], 50),
            "rhi_ms_p50": _pct([r["rhi_ms"] for r in sel], 50),
            "gpu_ms_p50": _pct([r["gpu_ms"] for r in sel], 50),
            "gpu_ms_p95": _pct([r["gpu_ms"] for r in sel], 95),
            "hitches_66": sum(1 for w in wall if w > HITCH_MS),
            "hitches_100": sum(1 for w in wall if w > BIG_HITCH_MS),
            "popin_fraction": (sum(1 for r in sel if r["load_pct"] < 100.0) / len(sel)) if sel else 0.0,
            "families_mean": (sum(r["families"] for r in sel) / len(sel)) if sel else 0.0,
            "dropped": (sel[-1]["dropped"] - sel[0]["dropped"]) if sel else 0,
        }
    return out
```

The `dropped` delta in the test (`0 → 3`) uses first and last rows. That matches the counter semantics (cumulative).

- [ ] **Step 6: Write `scripts/bench/compare.py`**

```python
"""Compare two bench results.json files; print a markdown table and shot SSIM (ROADMAP 3A).

Usage: uv run --with numpy --with pillow python scripts/bench/compare.py BASELINE_DIR CURRENT_DIR
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

METRICS = ["wall_ms_p50", "wall_ms_p95", "wall_ms_p99", "gpu_ms_p50", "gpu_ms_p95",
           "game_ms_p50", "render_ms_p50", "hitches_66", "hitches_100",
           "popin_fraction", "families_mean", "dropped"]


def _fmt(v) -> str:
    if v is None:
        return "—"
    return f"{v:.3f}" if isinstance(v, float) and abs(v) < 1 else f"{float(v):.1f}"


def _delta(a, b) -> str:
    if a is None or b is None:
        return "—"
    if a == 0:
        return "n/a" if b != 0 else "0%"
    return f"{(b - a) / a * 100.0:+.0f}%"


def render_table(baseline: dict, current: dict) -> str:
    bp, cp = baseline.get("phases", {}), current.get("phases", {})
    lines = ["| phase | metric | baseline | current | change |", "| --- | --- | --- | --- | --- |"]
    for phase in list(dict.fromkeys([*bp, *cp])):
        for m in METRICS:
            a, b = bp.get(phase, {}).get(m), cp.get(phase, {}).get(m)
            if a is None and b is None:
                continue
            lines.append(f"| {phase} | {m} | {_fmt(a)} | {_fmt(b)} | {_delta(a, b)} |")
    return "\n".join(lines)


def ssim(a: np.ndarray, b: np.ndarray) -> float:
    """Mean SSIM over 8x8 blocks of the luma channel (information only, not a gate)."""
    def luma(x):
        x = x.astype(np.float64)
        return 0.299 * x[..., 0] + 0.587 * x[..., 1] + 0.114 * x[..., 2] if x.ndim == 3 else x
    if a.shape != b.shape:
        return 0.0
    ya, yb = luma(a), luma(b)
    if np.array_equal(ya, yb):
        return 1.0
    h, w = (ya.shape[0] // 8) * 8, (ya.shape[1] // 8) * 8
    ba = ya[:h, :w].reshape(h // 8, 8, w // 8, 8).swapaxes(1, 2).reshape(-1, 64)
    bb = yb[:h, :w].reshape(h // 8, 8, w // 8, 8).swapaxes(1, 2).reshape(-1, 64)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = ba.mean(1), bb.mean(1)
    va, vb = ba.var(1), bb.var(1)
    cov = ((ba - ma[:, None]) * (bb - mb[:, None])).mean(1)
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma ** 2 + mb ** 2 + c1) * (va + vb + c2))
    return float(s.mean())


def main() -> int:
    from PIL import Image
    base_dir, cur_dir = Path(sys.argv[1]), Path(sys.argv[2])
    base = json.loads((base_dir / "results.json").read_text())
    cur = json.loads((cur_dir / "results.json").read_text())
    print(f"Baseline: {base.get('meta', {}).get('label')}  Current: {cur.get('meta', {}).get('label')}\n")
    print(render_table(base, cur))
    shots = sorted(p.name for p in (base_dir / "shots").glob("*.png"))
    if shots:
        print("\n| shot | SSIM |\n| --- | --- |")
        for name in shots:
            other = cur_dir / "shots" / name
            val = ssim(np.asarray(Image.open(base_dir / "shots" / name).convert("RGB")),
                       np.asarray(Image.open(other).convert("RGB"))) if other.exists() else None
            print(f"| {name} | {_fmt(val)} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 7: Make `bench` importable from the tests, and run them**

Create an empty `scripts/bench/__init__.py`. `scripts/tests/conftest.py` already puts `scripts/` on `sys.path`, so `from bench import scenario` works. `scenario.py` imports `send_cigi_test` as a top-level module, which also resolves through that path.

Run: `uv run --with pytest --with numpy --with pillow pytest scripts/tests/ -v`
Expected: all bench tests and existing tests pass.

- [ ] **Step 8: Write `scripts/bench/run_bench.py`**

```python
"""Run the render benchmark against a local CamSim (ROADMAP 3A).

Launches CamSim headless via scripts/run.sh with frame stats and /snapshot on,
flies scenario.py over CIGI, saves reference shots and the slew-phase stream,
then writes per-phase metrics.

Usage:
  uv run --with numpy --with pillow python scripts/bench/run_bench.py --label baseline
  ... --view-source scene_capture | --smoke | --trace | --skip-warmup | --out DIR
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from bench import analyze, scenario  # noqa: E402

HEALTH = f"http://127.0.0.1:{os.environ.get('CAMSIM_HEALTH_HTTP_PORT', '8080')}"
CIGI_PORT = int(os.environ.get("CAMSIM_CIGI_PORT", "8888"))
STREAM_PORT = int(os.environ.get("CAMSIM_MULTICAST_PORT", "5004"))
READY_TIMEOUT_S = 900       # a cold start compiles shaders
SHOT_SETTLE_S = 2.0         # TSR / Lumen history after the terrain gate opens


class Host:
    """Sends one CIGI host frame at 30 Hz for whatever pose is current."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.pose = scenario.build_phases()[1].pose_at(0.0)
        self.frame = 0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        period = 1.0 / 30.0
        next_t = time.monotonic()
        while not self.stop.is_set():
            self.sock.sendto(scenario.host_datagram(self.frame, self.pose), ("127.0.0.1", CIGI_PORT))
            self.frame += 1
            next_t += period
            time.sleep(max(0.0, next_t - time.monotonic()))


def http_json(path: str) -> dict | None:
    try:
        with urllib.request.urlopen(HEALTH + path, timeout=3) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read())
        except Exception:
            return None
    except Exception:
        return None


def wait_ready(pid_file: Path) -> None:
    deadline = time.time() + READY_TIMEOUT_S
    while time.time() < deadline:
        if pid_file.exists() and subprocess.run(["kill", "-0", pid_file.read_text().split()[0]],
                                                capture_output=True).returncode != 0:
            sys.exit("CamSim exited during startup")
        body = http_json("/ready")
        if body and body.get("status") == "ready":
            return
        time.sleep(2)
    sys.exit(f"/ready not reached within {READY_TIMEOUT_S}s")


def wait_terrain(timeout_s: float = 60.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        body = http_json("/ready")
        if body and body.get("terrain_ready"):
            return True
        time.sleep(0.5)
    return False


def fetch_snapshot(dest: Path) -> bool:
    try:
        with urllib.request.urlopen(HEALTH + "/snapshot", timeout=10) as r:
            dest.write_bytes(r.read())
            return True
    except Exception as e:  # noqa: BLE001 - report and carry on to the next shot
        print(f"[bench] snapshot {dest.name} failed: {e}")
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--view-source", choices=["primary", "scene_capture"], default=None)
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--trace", action="store_true", help="also record an Unreal Insights trace")
    ap.add_argument("--skip-warmup", action="store_true")
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    out = args.out or REPO / ".cache" / "bench" / f"{time.strftime('%Y%m%d-%H%M%S')}-{args.label}"
    (out / "shots").mkdir(parents=True, exist_ok=True)
    pid_file = REPO / ".cache" / "camsim.pid"

    env = dict(os.environ,
               CAMSIM_FRAME_STATS_PATH=str(out / "frames.jsonl"),
               CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1")
    if args.view_source:
        env["CAMSIM_RENDER_VIEW_SOURCE"] = args.view_source
    extra = [f"-trace=cpu,gpu,frame", f"-tracefile={out / 'trace.utrace'}"] if args.trace else []

    host = Host()
    host.thread.start()   # /ready needs CIGI traffic
    subprocess.run([str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach", *extra],
                   env=env, check=True)
    phases_log: list[dict] = []
    try:
        wait_ready(pid_file)
        for ph in scenario.build_phases(smoke=args.smoke):
            if ph.name == "warmup" and args.skip_warmup:
                continue
            print(f"[bench] phase {ph.name} ({ph.duration_s:.0f}s)")
            host.pose = ph.pose_at(0.0)
            if ph.measured:
                wait_terrain()          # start measuring from a loaded view
            ts = None
            if ph.name == "slew":
                ts = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error",
                                       "-i", f"udp://127.0.0.1:{STREAM_PORT}?timeout=5000000",
                                       "-t", str(ph.duration_s), "-c", "copy", str(out / "slew.ts")])
            start = time.time()
            while (t := time.time() - start) < ph.duration_s:
                host.pose = ph.pose_at(t)
                time.sleep(1.0 / 60.0)
            phases_log.append({"name": ph.name, "start": start, "end": time.time(), "measured": ph.measured})
            if ts:
                ts.wait(timeout=30)

        for shot in scenario.build_shots(smoke=args.smoke):
            host.pose = shot.pose
            time.sleep(1.0)             # let the pose arrive before checking the gate
            if not wait_terrain():
                print(f"[bench] shot {shot.name}: terrain gate never opened")
            time.sleep(SHOT_SETTLE_S)
            fetch_snapshot(out / "shots" / f"{shot.name}.png")
    finally:
        host.stop.set()
        subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False)

    (out / "phases.json").write_text(json.dumps(phases_log, indent=2))
    rows = analyze.load_rows(out / "frames.jsonl")
    sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, cwd=REPO).stdout.strip()
    results = {
        "meta": {"label": args.label, "git": sha, "platform": platform.platform(),
                 "machine": platform.machine(), "view_source": args.view_source or "config default",
                 "warmup_ran": not args.skip_warmup, "smoke": args.smoke},
        "phases": analyze.summarize(rows, phases_log),
    }
    (out / "results.json").write_text(json.dumps(results, indent=2))
    print(json.dumps(results["phases"], indent=2))
    print(f"[bench] results in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Check `scripts/stop.sh` stops the instance started with `--detach` (it reads `.cache/camsim.pid`). If `run.sh` doesn't forward `-trace=...` via `EXTRA_ARGS` (`scripts/run.sh:87`), pass it the way that file expects. Also confirm the GPU name for the metadata: add `"gpu": subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], ...)` on macOS only if it's useful. Apple silicon reports the SoC name there. Keep it optional.

- [ ] **Step 9: Write `scripts/bench/README.md`**

Cover:
- what the bench measures;
- the run command;
- the smoke command;
- how to compare two runs (`compare.py BASE CUR`);
- where baselines and shots live (`scripts/bench/baselines/`, `scripts/bench/shots/`);
- that it needs a Cesium ion token and network for the warm-up;
- that only warm-cache runs are comparable;
- that `families_mean` should be 1.0 on the primary path.

Keep it to about 40 lines.

- [ ] **Step 10: Commit**

```bash
git add scripts/send_cigi_test.py scripts/bench scripts/tests/test_bench_*.py scripts/tests/test_send_cigi.py
git commit -m "feat(bench): deterministic render benchmark and reference-shot harness (3A)"
```

---

### Task 5: Record the macOS baseline of today's pipeline

No code changes. This must run **before** Task 7 changes the render path.

**Files:**
- Create: `scripts/bench/baselines/macos-m1pro-baseline.json`, `scripts/bench/shots/macos/baseline/*.png`
- Modify: `.gitattributes`

- [ ] **Step 1: Track bench shots with LFS**

```bash
git lfs track "scripts/bench/shots/**/*.png"
git add .gitattributes
```

- [ ] **Step 2: Build and run a smoke pass**

```bash
scripts/run.sh --build-only
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label smoke --smoke
```

Expected: `results.json` with an `orbit` phase with `frames` > 400, one `nadir_3km.png` that isn't black, and `families_mean` recorded. If `/ready` never arrives, check `~/Library/Logs/CamSimTest/CamSimTest.log`. If multicast/unicast fails, `--local` sends to `127.0.0.1:5004`.

- [ ] **Step 3: Warm the cache, then record the baseline twice**

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label baseline-a
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label baseline-b --skip-warmup
uv run --with numpy --with pillow python scripts/bench/compare.py .cache/bench/*-baseline-a .cache/bench/*-baseline-b
```

Expected: run-to-run p50 differences under about 10%. If they're larger, note the noise level; it sets how big a change must be to count in Task 11.

- [ ] **Step 4: Store the baseline**

```bash
cp .cache/bench/*-baseline-b/results.json scripts/bench/baselines/macos-m1pro-baseline.json
mkdir -p scripts/bench/shots/macos/baseline
cp .cache/bench/*-baseline-b/shots/*.png scripts/bench/shots/macos/baseline/
```

Open two or three shots and check they look like San Francisco and the far area, not a black frame or the start location (Yuma).

- [ ] **Step 5: Commit**

```bash
git add scripts/bench/baselines scripts/bench/shots
git commit -m "bench: macOS M1 Pro baseline of the SceneCapture2D pipeline (3A)

families_mean=<value from results> (2 = the viewport renders the scene too)."
```

---

### Task 6: Grab request queue

**Files:**
- Create: `Camera/FrameGrabRequestQueue.h`
- Test: `Tests/RenderPathTest.cpp` (append)

**Interfaces:**
- Produces:
  - `struct FFrameGrabRequest { uint64 FrameIndex = 0; uint32 Generation = 0; int32 TargetIndex = INDEX_NONE; }`
  - `class FFrameGrabRequestQueue { void Push(const FFrameGrabRequest& R); bool PopCurrent(uint32 CurrentGeneration, FFrameGrabRequest& Out); int32 Num() const; }`. `PopCurrent` discards requests whose `Generation` is older than `CurrentGeneration` and returns the oldest current one. Owned and used by the render thread only.

- [ ] **Step 1: Write the failing test**

Append to `Tests/RenderPathTest.cpp` (add `#include "Camera/FrameGrabRequestQueue.h"` at the top):

```cpp
// The render thread grabs each requested frame exactly once, oldest first, and
// never a request the game thread has already given up on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameGrabQueueTest,
	"CamSim.Render.FrameGrab.RequestQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFrameGrabQueueTest::RunTest(const FString& Parameters)
{
	FFrameGrabRequestQueue Q;
	FFrameGrabRequest Out;
	TestFalse(TEXT("empty queue pops nothing"), Q.PopCurrent(1, Out));

	Q.Push({ 10, 1, 0 });
	Q.Push({ 11, 2, 1 });
	Q.Push({ 12, 3, 2 });

	TestTrue(TEXT("pops current"), Q.PopCurrent(3, Out));
	TestEqual(TEXT("stale generations 1-2 dropped, 3 returned"), Out.FrameIndex, (uint64)12);
	TestEqual(TEXT("queue drained"), Q.Num(), 0);

	Q.Push({ 20, 4, 0 });
	Q.Push({ 21, 4, 1 });
	TestTrue(TEXT("first of same generation"), Q.PopCurrent(4, Out));
	TestEqual(TEXT("FIFO"), Out.FrameIndex, (uint64)20);
	TestTrue(TEXT("second"), Q.PopCurrent(4, Out));
	TestEqual(TEXT("FIFO second"), Out.FrameIndex, (uint64)21);
	TestFalse(TEXT("each request grabbed once"), Q.PopCurrent(4, Out));
	return true;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `scripts/run.sh --build-only`
Expected: `'Camera/FrameGrabRequestQueue.h' file not found`.

- [ ] **Step 3: Implement**

Create `Camera/FrameGrabRequestQueue.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One frame the game thread wants copied out of the game viewport (ROADMAP 3A). */
struct FFrameGrabRequest
{
	uint64 FrameIndex  = 0;
	uint32 Generation  = 0;           // UCamSimCaptureComponent::PollGeneration at request time
	int32  TargetIndex = INDEX_NONE;  // grab render target / readback slot
};

/**
 * FIFO of grab requests. Render-thread only: the game thread reaches it through
 * ENQUEUE_RENDER_COMMAND, so no locking. Requests from an older generation were
 * abandoned by the game thread and are dropped rather than grabbed.
 */
class FFrameGrabRequestQueue
{
public:
	void Push(const FFrameGrabRequest& R) { Requests.Add(R); }

	bool PopCurrent(uint32 CurrentGeneration, FFrameGrabRequest& Out)
	{
		while (Requests.Num() > 0)
		{
			const FFrameGrabRequest Front = Requests[0];
			Requests.RemoveAt(0, EAllowShrinking::No);
			if (Front.Generation >= CurrentGeneration)
			{
				Out = Front;
				return true;
			}
		}
		return false;
	}

	int32 Num() const { return Requests.Num(); }

private:
	TArray<FFrameGrabRequest> Requests;
};
```

If `EAllowShrinking` doesn't compile in 5.8, use `RemoveAt(0, 1, EAllowShrinking::No)` or the plain `RemoveAt(0)`. The queue holds at most a few entries.

- [ ] **Step 4: Build and run**

Run: `scripts/run.sh --build-only`, then headless `RunTests CamSim.Render`.
Expected: `CamSim.Render.FrameGrab.RequestQueue` passes.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/FrameGrabRequestQueue.h \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/RenderPathTest.cpp
git commit -m "feat(camera): frame grab request queue (3A)"
```

---

### Task 7: Primary-view render path

This is the core switch. Step 1 is a **go/no-go check on Metal**. Don't build further until it passes.

**Files:**
- Create: `Camera/CamSimFrameGrabExtension.h`, `Camera/CamSimFrameGrabExtension.cpp`
- Modify: `Camera/CamSimCamera.h/.cpp` (camera component, view target, viewport size, per-tick copy)
- Modify: `Camera/CamSimCaptureComponent.h/.cpp` (primary-mode `Capture()`, show flags, AA)
- Modify: `Camera/CamSimStreamingController.h/.cpp` (no primary Cesium camera in primary mode)
- Modify: `Config/CamSimConfig.h` / `deploy/camsim_config.yaml` / `docs/configuration.md` (`use_lod_transitions` default → `true`)

**Interfaces:**
- Consumes: `FFrameGrabRequestQueue` (Task 6); `FCamSimConfig::Render` (Task 1).
- Produces:
  - `class FCamSimFrameGrabExtension : public FSceneViewExtensionBase` with ctor `(const FAutoRegister&, FViewport* InGameViewport)`, `void PushRequest_RenderThread(const FFrameGrabRequest& R, FRHITexture* Target, FRHIGPUTextureReadback* Readback)`, `void SetCurrentGeneration_RenderThread(uint32 Gen)`, `void Detach_GameThread()`.
  - `ACamSimCamera::SensorCamera` (`UCameraComponent*`).
  - `FCamSimStreamingController::Initialize(AActor* Owner, const FCamSimConfig& Cfg)`, unchanged signature: it reads `Cfg.Render.IsPrimary()` to skip the primary slot.

- [ ] **Step 1: Go/no-go check: can we grab the game viewport on Metal under `-RenderOffScreen`?**

Write `Camera/CamSimFrameGrabExtension.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Camera/FrameGrabRequestQueue.h"

class FRHIGPUTextureReadback;
class FViewport;

/**
 * FCamSimFrameGrabExtension (ROADMAP 3A)
 *
 * When the sensor is the primary view, copies the game viewport's final image
 * into a capture-sized grab target and queues the async readback, once per
 * requested frame. Scene captures (Context.Viewport == null) and other
 * viewports are ignored.
 */
class FCamSimFrameGrabExtension : public FSceneViewExtensionBase
{
public:
	FCamSimFrameGrabExtension(const FAutoRegister& AutoRegister, FViewport* InGameViewport);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

	/** Render thread: grab the next game-viewport frame into Target, then EnqueueCopy into Readback. */
	void PushRequest_RenderThread(const FFrameGrabRequest& R, FRHITexture* Target, FRHIGPUTextureReadback* Readback);
	void SetCurrentGeneration_RenderThread(uint32 Gen) { CurrentGeneration = Gen; }

	/** Game thread: stop matching any viewport (before the owner is destroyed). */
	void Detach_GameThread() { GameViewport.Store(nullptr); }

	/** Frames grabbed so far; read by the go/no-go check and logs. */
	uint64 GetGrabCount() const { return GrabCount.Load(EMemoryOrder::Relaxed); }

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	struct FTargets { FRHITexture* Target = nullptr; FRHIGPUTextureReadback* Readback = nullptr; };

	TAtomic<FViewport*> GameViewport { nullptr };
	FFrameGrabRequestQueue Requests;              // render thread
	TMap<int32, FTargets>  TargetsBySlot;         // render thread
	uint32 CurrentGeneration = 0;                 // render thread
	TAtomic<uint64> GrabCount { 0 };
};
```

Write `Camera/CamSimFrameGrabExtension.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimFrameGrabExtension.h"
#include "CamSimTest.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "SceneView.h"
#include "UnrealClient.h"

FCamSimFrameGrabExtension::FCamSimFrameGrabExtension(const FAutoRegister& AutoRegister, FViewport* InGameViewport)
	: FSceneViewExtensionBase(AutoRegister)
{
	GameViewport.Store(InGameViewport);
}

bool FCamSimFrameGrabExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	FViewport* Target = GameViewport.Load();
	return Target != nullptr && Context.Viewport == Target;
}

void FCamSimFrameGrabExtension::PushRequest_RenderThread(const FFrameGrabRequest& R, FRHITexture* Target,
	FRHIGPUTextureReadback* Readback)
{
	check(IsInRenderingThread());
	TargetsBySlot.Add(R.TargetIndex, { Target, Readback });
	Requests.Push(R);
}

void FCamSimFrameGrabExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	FFrameGrabRequest Req;
	if (!Requests.PopCurrent(CurrentGeneration, Req)) return;
	const FTargets* T = TargetsBySlot.Find(Req.TargetIndex);
	if (!T || !T->Target || !T->Readback || !InViewFamily.RenderTarget || InViewFamily.Views.Num() == 0) return;

	FRDGTextureRef Source = InViewFamily.RenderTarget->GetRenderTargetTexture(GraphBuilder);
	if (!Source) return;
	FRDGTextureRef Dest = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(T->Target, TEXT("CamSimGrabTarget")));

	// The view rect, not the whole backbuffer: the viewport may be larger than
	// the view (DPI scale, window chrome). The draw scales to the grab target.
	const FSceneView& View = *InViewFamily.Views[0];
	const FIntRect SrcRect = View.UnscaledViewRect;
	AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(View), Source, Dest,
		SrcRect.Min, SrcRect.Size(), FIntPoint::ZeroValue, Dest->Desc.Extent);

	FRHIGPUTextureReadback* Readback = T->Readback;
	AddEnqueueCopyPass(GraphBuilder, Readback, Dest);
	GrabCount.Store(GrabCount.Load(EMemoryOrder::Relaxed) + 1, EMemoryOrder::Relaxed);
}
```

Check two things against the engine before building:
1. `AddEnqueueCopyPass(FRDGBuilder&, FRHIGPUTextureReadback*, FRDGTextureRef)` exists: `grep -rn "AddEnqueueCopyPass" "/Users/Shared/Epic Games/UE_5.8/Engine/Source/Runtime/RenderCore/Public/"`. If not, use a pass with `ERDGPassFlags::Readback` calling `Readback->EnqueueCopy(RHICmdList, Dest->GetRHI())`.
2. `FSceneView::UnscaledViewRect` exists. If not, use `View.UnconstrainedViewRect`.

Add `"Renderer"` includes via the Build.cs module list: `Renderer`, `RenderCore` and `RHI` are already there.

**Go/no-go:** register the extension and push a request from `ACamSimCamera` behind the switch (the next steps do this properly). The quickest check is to finish Steps 2–4 and then run Step 7's smoke. The check passes when:
- a snapshot (Task 3) in primary mode is the same size as the capture;
- it isn't black and shows the sensor's view (compare against the Task 5 baseline `nadir_3km.png`);
- its brightness is close to the scene-capture version: mean luma within 15% (`python3 -c` with PIL over both files). A much darker image means the backbuffer is sRGB and the draw linearised it. Fix that by creating the grab targets with `bInForceLinearGamma=true` (Step 3). If that doesn't match, fix it with an sRGB-aware draw.

If `GetRenderTargetTexture(GraphBuilder)` returns null under `-RenderOffScreen` on Metal, **stop and report**. The fallback is `FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent()`, the hook Pixel Streaming uses, which needs its own design pass.

- [ ] **Step 2: Add the camera component and view target**

In `Camera/CamSimCamera.h`: forward-declare `class UCameraComponent;` and `class FCamSimFrameGrabExtension;`. Add:

```cpp
	/**
	 * The player's view when render.view_source = primary (ROADMAP 3A). Child of
	 * SceneCapture with an identity transform; FOV and post-process are copied
	 * from SceneCapture every tick, which stays the source of truth until 3B.
	 */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCameraComponent> SensorCamera;

	bool bViewTargetApplied = false;
	void ApplyPrimaryView();
```

In `Camera/CamSimCamera.cpp` constructor, after the `SceneCapture` setup:

```cpp
	SensorCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("SensorCamera"));
	SensorCamera->SetupAttachment(SceneCapture);
	SensorCamera->bConstrainAspectRatio = false;
	SensorCamera->bUsePawnControlRotation = false;
```

Include `Camera/CameraComponent.h`, `Kismet/GameplayStatics.h`, `GameFramework/PlayerController.h` and `UnrealEngine.h`.

In `BeginPlay()`, before `CaptureComp->Initialize(...)`:

```cpp
	if (Cfg.Render.IsPrimary())
	{
		// The game viewport renders at the stream resolution (ROADMAP 3A).
		FSystemResolution::RequestResolutionChange(Cfg.CaptureWidth, Cfg.CaptureHeight, EWindowMode::Windowed);
	}
	else if (UGameViewportClient* GVC = GetWorld()->GetGameViewport())
	{
		// Legacy path: don't pay for a second, unused render of the world.
		GVC->bDisableWorldRendering = true;
	}
```

Change the `CaptureComp->Initialize` call to pass the camera: `CaptureComp->Initialize(SceneCapture, SensorCamera, Subsystem, Cfg);`.

New function, called at the end of `ApplyCigiViewState()` and after `UpdateAutoFocus()` in `Tick()`:

```cpp
void ACamSimCamera::ApplyPrimaryView()
{
	if (!Subsystem || !Subsystem->GetConfig().Render.IsPrimary()) return;

	// SceneCapture holds pose (via attachment), FOV and post-process; mirror them.
	SensorCamera->SetFieldOfView(SceneCapture->FOVAngle);
	SensorCamera->PostProcessSettings = SceneCapture->PostProcessSettings;
	SensorCamera->PostProcessBlendWeight = 1.0f;

	if (!bViewTargetApplied)
	{
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->SetViewTarget(this);
			bViewTargetApplied = true;
			UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: sensor is the primary view (%s)"), *PC->GetName());
		}
	}
}
```

Copying `FPostProcessSettings` each tick costs a few KB of memcpy, which is fine at 30 Hz. With an actor as view target, `APlayerCameraManager` uses the actor's active `UCameraComponent`. If the view doesn't follow the gimbal, check `SensorCamera->bAutoActivate` and that `CalcCamera` picks it (`AActor::bFindCameraComponentWhenViewTarget` defaults to true).

- [ ] **Step 3: Capture through the grab extension in primary mode**

In `Camera/CamSimCaptureComponent.h`:
- change `Initialize` to `void Initialize(USceneCaptureComponent2D* InSensor, UCameraComponent* InCamera, UCamSimSubsystem* InSubsystem, const FCamSimConfig& Cfg);`;
- forward-declare `class UCameraComponent;` and `class FCamSimFrameGrabExtension;`;
- add members:

```cpp
	/** ROADMAP 3A: primary-view grab (null in scene_capture mode). */
	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> GrabExtension;
	bool bPrimaryView = false;
```

In `Camera/CamSimCaptureComponent.cpp` `Initialize`, after the render targets are created:

```cpp
	bPrimaryView = Cfg.Render.IsPrimary();
	if (bPrimaryView)
	{
		FViewport* Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		GrabExtension = FSceneViewExtensions::NewExtension<FCamSimFrameGrabExtension>(Viewport);
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: primary view — grabbing the game viewport (%s)"),
			Viewport ? TEXT("ok") : TEXT("NO VIEWPORT"));
	}
```

`GEngine->GameViewport->Viewport` may be null at `BeginPlay` in some launch modes. If so, create the extension lazily in `Capture()` the first time the viewport exists.

In `Shutdown()`, before `FlushRenderingCommands()`:

```cpp
	if (GrabExtension) { GrabExtension->Detach_GameThread(); }
```

After the flush: `GrabExtension.Reset();`.

In `Capture()`, replace the block from `Sensor->TextureTarget = RT;` through the `ENQUEUE_RENDER_COMMAND(CamSimEnqueueReadback)` lambda. Keep the state-machine lines between them exactly as they are:

```cpp
	UTextureRenderTarget2D* RT = RenderTargets[CaptureTargetIndex].Get();
	FRHIGPUTextureReadback* Readback = ColorReadbackPool[CaptureTargetIndex].Get();
	const int32 TargetIdx = CaptureTargetIndex;
	if (!bPrimaryView)
	{
		Sensor->TextureTarget = RT;
		Sensor->CaptureScene();
	}
	PendingReadbackTargetIndex = CaptureTargetIndex;
	CaptureTargetIndex = (CaptureTargetIndex + 1) % RenderTargets.Num();

	PendingFrameIndex = FrameIndex++;
	PendingTelemetry  = Telemetry;

	PollGeneration.Store(PollGeneration.Load(EMemoryOrder::Relaxed) + 1, EMemoryOrder::SequentiallyConsistent);
	RenderReadyStreak     .Store(0, EMemoryOrder::Relaxed);
	RenderDepthReadyStreak.Store(0, EMemoryOrder::Relaxed);
	ReadbackState.Store(EReadbackState::DMAQueued, EMemoryOrder::SequentiallyConsistent);

	// (depth capture block unchanged)

	if (bPrimaryView)
	{
		// The game viewport renders after this tick; the extension copies that
		// frame into RT and queues the readback. Enqueued now, so it's in the
		// queue before this frame's scene render command.
		const uint32 Gen = PollGeneration.Load(EMemoryOrder::Relaxed);
		const uint64 FrameIdx = PendingFrameIndex;
		TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> Ext = GrabExtension;
		ENQUEUE_RENDER_COMMAND(CamSimRequestGrab)(
			[Ext, RT, Readback, Gen, FrameIdx, TargetIdx, DepthRT, DepthReadback](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRenderTargetResource* Resource = RT->GetRenderTargetResource();
			if (!Ext || !Resource) return;
			Ext->SetCurrentGeneration_RenderThread(Gen);
			Ext->PushRequest_RenderThread({ FrameIdx, Gen, TargetIdx }, Resource->GetRenderTargetTexture(), Readback);
			// Depth (ML) still comes from its own SceneCapture, copied as before.
			if (DepthRT && DepthReadback)
			{
				FTextureRenderTargetResource* DepthRes = DepthRT->GetRenderTargetResource();
				if (FRHITexture* DepthTex = DepthRes ? DepthRes->GetRenderTargetTexture() : nullptr)
				{
					RHICmdList.Transition(FRHITransitionInfo(DepthTex, ERHIAccess::RTV, ERHIAccess::CopySrc));
					DepthReadback->EnqueueCopy(RHICmdList, DepthTex);
					RHICmdList.Transition(FRHITransitionInfo(DepthTex, ERHIAccess::CopySrc, ERHIAccess::RTV));
				}
			}
		});
		return;
	}

	// scene_capture: the existing CamSimEnqueueReadback render command, unchanged.
```

The poll path (`EnqueuePoll`) is unchanged: it waits on `Readback->IsReady()`, which becomes true once the extension's copy completes. `SnapshotGroundTruthEntities()` keeps reading `Sensor`, which still holds the pose.

In `ApplyRenderSettings`, make the show flags and AA depend on the mode. Right after each `Sensor->ShowFlags.SetX(v)` call (motion blur, bloom, lens flares, contact shadows), mirror it to the game viewport in primary mode. Add at the top of the function:

```cpp
	FEngineShowFlags* ViewFlags = (Cfg.Render.IsPrimary() && GEngine && GEngine->GameViewport)
		? &GEngine->GameViewport->EngineShowFlags : nullptr;
```

Then after each set, e.g. `Sensor->ShowFlags.SetBloom(O.bBloom);`, add `if (ViewFlags) ViewFlags->SetBloom(O.bBloom);`. Do the same for `SetMotionBlur`, `SetLensFlares` and `SetContactShadows`.

Replace:

```cpp
		// FXAA (1): TSR (4) ghosts on off-screen captures; FXAA is stateless.
		SetCVarI(TEXT("r.AntiAliasingMethod"), 1);
```

with:

```cpp
		// Primary view: TSR (4) with full view history. SceneCapture: FXAA (1),
		// because TSR ghosts on off-screen captures (no persistent history).
		SetCVarI(TEXT("r.AntiAliasingMethod"), Cfg.Render.IsPrimary() ? 4 : 1);
```

Update the log line's `AA=FXAA` to print `Cfg.Render.IsPrimary() ? TEXT("TSR") : TEXT("FXAA")`.

In `Initialize`, when creating the ring's render targets, use the gamma decided by the go/no-go in Step 1:
- `bInForceLinearGamma=false` stays for scene_capture;
- for primary, use whatever made the brightness match.

Note the result in a comment.

- [ ] **Step 4: Cesium: drop the primary stand-in camera, enable LOD transitions**

In `Camera/CamSimStreamingController.cpp` `Initialize`, wrap the primary registration:

```cpp
	// Primary view: Cesium already selects tiles for the player camera, so only
	// the inflated prefetch camera is added. SceneCapture needs both.
	if (!Cfg.Render.IsPrimary())
	{
		PrimaryCameraSlot = CamMgr->AdditionalCameras.Add(
			FCesiumCamera(Viewport, Owner->GetActorLocation(), Owner->GetActorRotation(), Cfg.HFovDeg));
	}
```

`Shutdown` and `UpdateCameras` already skip invalid slots (`IsValidIndex(-1)` is false). Removing "the higher slot first" still works with one slot.

Add a test to `Tests/RenderPathTest.cpp` that pins the slot rule without a world. Expose a static helper in `CamSimStreamingController.h`:

```cpp
	/** Cesium stand-in cameras this view source needs: primary + prefetch, or prefetch only. */
	static int32 NumStreamingCameras(const FCamSimConfig& Cfg) { return Cfg.Render.IsPrimary() ? 1 : 2; }
```

Use it in `Initialize`'s log line, and test it:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamingCamerasTest,
	"CamSim.Render.Streaming.CamerasPerViewSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStreamingCamerasTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	TestEqual(TEXT("primary: prefetch only"), FCamSimStreamingController::NumStreamingCameras(Cfg), 1);
	Cfg.Render.ViewSourceMode = FCamSimConfig::FRenderConfig::EViewSource::SceneCapture;
	TestEqual(TEXT("scene capture: primary + prefetch"), FCamSimStreamingController::NumStreamingCameras(Cfg), 2);
	return true;
}
```

Include `Camera/CamSimStreamingController.h` and `Config/CamSimConfig.h` in the test file.

LOD transitions:
- In `Config/CamSimConfig.h`, set `bool bUseLodTransitions = true;` and replace its comment with: "Cesium's dithered LOD crossfade. Needs temporal AA to resolve, so it only looks right with render.view_source = primary (TSR)".
- Set `LodTransitionLength = 0.5f` as the starting value; Task 11 tunes it.
- In `deploy/camsim_config.yaml`, set `use_lod_transitions: true` and `lod_transition_length: 0.5`, and update both comments the same way.
- In `docs/configuration.md`, update both defaults.
- Check that the existing config tests don't pin the old default: `grep -rn "UseLodTransitions\|LodTransitionLength" Tests/`.

- [ ] **Step 5: Build and run all automation tests**

Run: `scripts/run.sh --build-only`, then headless `RunTests CamSim`.
Expected: all tests pass, including `CamSim.Render.Streaming.CamerasPerViewSource`. Existing camera tests that construct `UCamSimCaptureComponent::Initialize` directly need the new argument (`grep -rn "CaptureComp->Initialize\|->Initialize(.*Subsystem" Tests/`). Pass `nullptr` for the camera there.

- [ ] **Step 6: Run the go/no-go smoke in both modes**

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label primary-smoke --smoke --skip-warmup
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label capture-smoke --smoke --skip-warmup --view-source scene_capture
```

Expected:
- `primary-smoke`: `families_mean` = 1.0, `nadir_3km.png` is capture-sized, not black, and its mean luma is within 15% of `capture-smoke`'s.
- `capture-smoke`: `families_mean` = 1.0 as well (world rendering disabled on the viewport), where the Task 5 baseline shows 2.

Also run `scripts/test_video_output.sh` or play the stream (`ffplay udp://@127.0.0.1:5004`) while a primary run is live. The encoded video must show the sensor view and KLV must still pass: `node scripts/klv_conformance/check.js udp://127.0.0.1:5004`. Check that script's `--help` for the exact input form.

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
        unreal_project/CamSimTest/Source/CamSimTest/Tests \
        deploy/camsim_config.yaml docs/configuration.md
git commit -m "feat(camera): render the sensor as the primary view with TSR (3A)

One scene render per frame (families 2 -> 1), TSR instead of FXAA, Cesium
LOD transitions on. render.view_source=scene_capture keeps the old path
(now without the unused viewport render) for A/B until 3B."
```

---

### Task 8: TSR camera cuts on pose jumps

**Files:**
- Create: `Camera/CamSimRenderPath.h`
- Modify: `Camera/CamSimCamera.h/.cpp`
- Test: `Tests/RenderPathTest.cpp` (append)

**Interfaces:**
- Consumes: `FCamSimConfig::Render.CameraCutDistanceM/CameraCutAngleDeg` (Task 1); `ACamSimCamera::bCameraCutThisFrame` (Task 2).
- Produces: `bool CamSimRender::ShouldCutCamera(const FVector& PrevLocCm, const FQuat& PrevRot, const FVector& CurLocCm, const FQuat& CurRot, double CutDistanceM, double CutAngleDeg)`.

- [ ] **Step 1: Write the failing test**

Append to `Tests/RenderPathTest.cpp` (include `Camera/CamSimRenderPath.h`):

```cpp
// A pose jump beyond either threshold in one frame resets TSR history. This
// covers CIGI teleports and Cesium origin rebases, which move the camera's UE
// location by kilometres even though the view doesn't change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraCutThresholdTest,
	"CamSim.Render.CameraCut.Thresholds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraCutThresholdTest::RunTest(const FString& Parameters)
{
	using CamSimRender::ShouldCutCamera;
	const FQuat Level = FRotator(-30.0, 0.0, 0.0).Quaternion();
	const FVector Origin(0.0);
	const double DistM = 500.0, AngDeg = 30.0;

	TestFalse(TEXT("still"), ShouldCutCamera(Origin, Level, Origin, Level, DistM, AngDeg));
	// 100 m/s at 30 fps = 3.3 m per frame: normal flight.
	TestFalse(TEXT("normal flight"), ShouldCutCamera(Origin, Level, FVector(333.0, 0, 0), Level, DistM, AngDeg));
	// 60 deg/s gimbal slew = 2 deg per frame.
	TestFalse(TEXT("fast slew"), ShouldCutCamera(Origin, Level, Origin, FRotator(-30.0, 2.0, 0.0).Quaternion(), DistM, AngDeg));
	// Teleport 300 km.
	TestTrue(TEXT("teleport"), ShouldCutCamera(Origin, Level, FVector(3.0e7, 0, 0), Level, DistM, AngDeg));
	// Origin rebase: the camera's UE location jumps back toward zero.
	TestTrue(TEXT("rebase"), ShouldCutCamera(FVector(2.0e6, -1.5e6, 3.0e5), Level, FVector(0, 0, 3.0e5), Level, DistM, AngDeg));
	// Snap the view 90 deg.
	TestTrue(TEXT("view snap"), ShouldCutCamera(Origin, Level, Origin, FRotator(-30.0, 90.0, 0.0).Quaternion(), DistM, AngDeg));
	// Just under / just over the distance threshold (cm).
	TestFalse(TEXT("499 m"), ShouldCutCamera(Origin, Level, FVector(49900.0, 0, 0), Level, DistM, AngDeg));
	TestTrue(TEXT("501 m"), ShouldCutCamera(Origin, Level, FVector(50100.0, 0, 0), Level, DistM, AngDeg));
	return true;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `scripts/run.sh --build-only`
Expected: `'Camera/CamSimRenderPath.h' file not found`.

- [ ] **Step 3: Implement the helper**

Create `Camera/CamSimRenderPath.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace CamSimRender
{
	/**
	 * True when the view moved too far in one frame for TSR to reproject
	 * (teleport, origin rebase, view snap). Locations in UE units (cm).
	 */
	inline bool ShouldCutCamera(const FVector& PrevLocCm, const FQuat& PrevRot,
	                            const FVector& CurLocCm, const FQuat& CurRot,
	                            double CutDistanceM, double CutAngleDeg)
	{
		const double MovedM = FVector::Dist(PrevLocCm, CurLocCm) / 100.0;
		const double TurnedDeg = FMath::RadiansToDegrees(PrevRot.AngularDistance(CurRot));
		return MovedM > CutDistanceM || TurnedDeg > CutAngleDeg;
	}
}
```

- [ ] **Step 4: Wire it into the camera**

In `Camera/CamSimCamera.h`, add private members:

```cpp
	FVector PrevViewLocCm = FVector::ZeroVector;
	FQuat   PrevViewRot   = FQuat::Identity;
	bool    bHasPrevView  = false;
	void UpdateCameraCut();
```

In `Camera/CamSimCamera.cpp` (include `Camera/CamSimRenderPath.h` and `Camera/PlayerCameraManager.h`), call `UpdateCameraCut();` in `Tick()` right after `ApplyPrimaryView()`:

```cpp
void ACamSimCamera::UpdateCameraCut()
{
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	const FVector Loc = SceneCapture->GetComponentLocation();
	const FQuat   Rot = SceneCapture->GetComponentQuat();
	if (bHasPrevView && CamSimRender::ShouldCutCamera(PrevViewLocCm, PrevViewRot, Loc, Rot,
		Cfg.Render.CameraCutDistanceM, Cfg.Render.CameraCutAngleDeg))
	{
		bCameraCutThisFrame = true;
		if (Cfg.Render.IsPrimary())
		{
			if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
			{
				if (PC->PlayerCameraManager) PC->PlayerCameraManager->SetGameCameraCutThisFrame();
			}
		}
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: camera cut (moved %.0f m)"), FVector::Dist(PrevViewLocCm, Loc) / 100.0);
	}
	PrevViewLocCm = Loc;
	PrevViewRot   = Rot;
	bHasPrevView  = true;
}
```

- [ ] **Step 5: Build, test, commit**

Run: `scripts/run.sh --build-only`, then headless `RunTests CamSim.Render`.
Expected: `CamSim.Render.CameraCut.Thresholds` passes.

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimRenderPath.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/RenderPathTest.cpp
git commit -m "feat(camera): reset TSR history on pose jumps (3A)"
```

---

### Task 9: Cesium origin shift

**Files:**
- Modify: `Camera/CamSimCamera.h/.cpp`
- Modify: `deploy/camsim_config.yaml`, `docs/configuration.md` (default after tuning, Task 11)

**Interfaces:**
- Consumes: `FCamSimConfig::Render.OriginShiftDistanceM` (Task 1); camera cuts (Task 8) catch the rebase jump automatically.
- Produces: `ACamSimCamera::OriginShift` (`UCesiumOriginShiftComponent*`), active only when the distance is > 0.

- [ ] **Step 1: Audit what a rebase would move**

With `ChangeCesiumGeoreference`, everything **not** anchored with a `UCesiumGlobeAnchorComponent` appears to move when the origin shifts. Run:

```bash
cd unreal_project/CamSimTest/Source/CamSimTest
grep -rln "SpawnActor" --include=*.cpp . | xargs grep -L "GlobeAnchor"
grep -rn "GetActorLocation()\|GetComponentLocation()" --include=*.h . | grep -v Tests/
```

Write a list in the commit message of the actors spawned without a globe anchor, and of any class that caches a UE world position across frames (member `FVector` holding a location). For each, decide one of:
- (a) it's anchored or recomputed every frame, so it's fine;
- (b) it needs a globe anchor;
- (c) it caches positions, so re-derive them from geodetic each frame or on the georeference's `OnGeoreferenceUpdated` event.

Known to check:
- `FCamSimStreamingController` gate position (it stores lat/lon, so fine);
- `FCamSimTelemetryAssembler` (frame-centre traces are per frame, so fine);
- entity dead-reckoning (it should work in geodetic);
- ocean and particle managers;
- `ACamSimEnvironment`. `CesiumSunSky` follows the georeference.

Fix every (b) and (c) case in this task.

- [ ] **Step 2: Add the component**

In `Camera/CamSimCamera.h`, forward-declare `class UCesiumOriginShiftComponent;` and add:

```cpp
	/** Rebases the georeference as the sensor travels (ROADMAP 3A); inactive when distance = 0. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCesiumOriginShiftComponent> OriginShift;
```

In the constructor:

```cpp
	OriginShift = CreateDefaultSubobject<UCesiumOriginShiftComponent>(TEXT("OriginShift"));
	OriginShift->SetMode(ECesiumOriginShiftMode::Disabled);  // configured in BeginPlay
```

Include `CesiumOriginShiftComponent.h` and `Cesium3DTileset.h`.

In `BeginPlay()`, after `Streaming.Initialize(...)`:

```cpp
	if (Cfg.Render.OriginShiftDistanceM > 0.0)
	{
		// ChangeCesiumGeoreference moves tilesets, so they must be Movable.
		for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
		{
			if (USceneComponent* TilesetRoot = It->GetRootComponent())
			{
				TilesetRoot->SetMobility(EComponentMobility::Movable);
			}
		}
		OriginShift->SetDistance(Cfg.Render.OriginShiftDistanceM * 100.0);  // m -> UE cm
		OriginShift->SetMode(ECesiumOriginShiftMode::ChangeCesiumGeoreference);
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: origin shift every %.0f m"), Cfg.Render.OriginShiftDistanceM);
	}
```

Check the unit of `UCesiumOriginShiftComponent::Distance` in its header comment (Cesium documents it in "Unreal units" = cm; if it says metres, drop the `* 100.0`). `grep -n "Distance" "/Users/Shared/Epic Games/UE_5.8/Engine/Plugins/Marketplace/CesiumForUnreal/Source/CesiumRuntime/Public/CesiumOriginShiftComponent.h"`.

The component shifts relative to its owner's location. The owner is the platform actor, not the gimballed sensor; that's fine for a threshold in kilometres.

- [ ] **Step 3: Verify with a far-origin run**

```bash
CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M=20000 \
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label origin-shift --skip-warmup
```

Expected:
- the log shows `origin shift every 20000 m`, and at the teleport a camera cut is logged (moved ≫ 500 m);
- `far_origin_slant.png` and `far_origin_nadir.png` have a level horizon and daylight comparable to `slant_10km.png`;
- entities and KLV are unaffected. KLV frame centre and sensor position come from geodetic telemetry. Spot-check with `node scripts/klv_conformance/check.js` during the far phase.

Compare the far-origin shots against the Task 5 baseline shots side by side. Tilted horizon, wrong sun or odd haze in the baseline, fixed here, is the evidence for the spec's "never quite right" hypothesis. Note what you see in the commit message.

- [ ] **Step 4: Build, run all tests, commit**

Run: `scripts/run.sh --build-only`, then headless `RunTests CamSim`. All tests must pass.

```bash
git add unreal_project/CamSimTest/Source/CamSimTest
git commit -m "feat(camera): Cesium origin shift behind render.origin_shift_distance_m (3A)

Audit of unanchored actors / cached world positions: <list and fixes>."
```

---

### Task 10: Real-RHI smoke in `ci_validate.sh --native`

**Files:**
- Modify: `scripts/ci_validate.sh`
- Create: `scripts/bench/check_smoke.py`
- Test: `scripts/tests/test_bench_check_smoke.py`

**Interfaces:**
- Consumes: `run_bench.py --smoke --out DIR` (Task 4).
- Produces: `check_smoke.check(out_dir: Path, width: int, height: int) -> list[str]` (failure messages; empty = pass). CLI `python scripts/bench/check_smoke.py DIR WIDTH HEIGHT` exits 1 on any failure.

- [ ] **Step 1: Write the failing test**

`scripts/tests/test_bench_check_smoke.py`:

```python
"""The smoke checker fails on missing, wrong-size or black snapshots and on double renders."""
import json

import numpy as np
from PIL import Image

from bench import check_smoke


def _write(tmp_path, img=None, families=1.0):
    (tmp_path / "shots").mkdir()
    (tmp_path / "results.json").write_text(json.dumps({"phases": {"orbit": {"frames": 500, "families_mean": families}}}))
    if img is not None:
        Image.fromarray(img).save(tmp_path / "shots" / "nadir_3km.png")


def test_good_run_passes(tmp_path):
    rng = np.random.default_rng(1)
    _write(tmp_path, rng.integers(20, 230, (72, 128, 3), dtype=np.uint8))
    assert check_smoke.check(tmp_path, 128, 72) == []


def test_missing_snapshot_fails(tmp_path):
    _write(tmp_path)
    assert any("no snapshot" in m for m in check_smoke.check(tmp_path, 128, 72))


def test_black_frame_fails(tmp_path):
    _write(tmp_path, np.zeros((72, 128, 3), dtype=np.uint8))
    assert any("black" in m for m in check_smoke.check(tmp_path, 128, 72))


def test_wrong_size_fails(tmp_path):
    _write(tmp_path, np.full((36, 64, 3), 128, dtype=np.uint8))
    assert any("size" in m for m in check_smoke.check(tmp_path, 128, 72))


def test_double_render_fails(tmp_path):
    rng = np.random.default_rng(1)
    _write(tmp_path, rng.integers(20, 230, (72, 128, 3), dtype=np.uint8), families=2.0)
    assert any("families" in m for m in check_smoke.check(tmp_path, 128, 72))
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `uv run --with pytest --with numpy --with pillow pytest scripts/tests/test_bench_check_smoke.py -v`
Expected: `ImportError: cannot import name 'check_smoke'`.

- [ ] **Step 3: Implement `scripts/bench/check_smoke.py`**

```python
"""Pass/fail checks for run_bench.py --smoke output (used by ci_validate.sh --native)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

MIN_MEAN_LUMA = 8.0      # 0-255; a black or failed grab is ~0
MAX_FAMILIES = 1.05      # one scene render per frame on the primary path


def check(out_dir: Path, width: int, height: int) -> list[str]:
    errors: list[str] = []
    shots = sorted((out_dir / "shots").glob("*.png"))
    if not shots:
        errors.append("no snapshot was saved (GET /snapshot never answered 200)")
    for shot in shots:
        img = np.asarray(Image.open(shot).convert("RGB"))
        if img.shape[1] != width or img.shape[0] != height:
            errors.append(f"{shot.name}: size {img.shape[1]}x{img.shape[0]}, expected {width}x{height}")
        if img.mean() < MIN_MEAN_LUMA:
            errors.append(f"{shot.name}: black frame (mean {img.mean():.1f})")
    phases = json.loads((out_dir / "results.json").read_text()).get("phases", {})
    for name, m in phases.items():
        if m.get("frames", 0) == 0:
            errors.append(f"{name}: no frame stats recorded")
        if m.get("families_mean", 0.0) > MAX_FAMILIES:
            errors.append(f"{name}: families_mean {m['families_mean']:.2f} > 1 (scene rendered more than once per frame)")
    return errors


if __name__ == "__main__":
    errs = check(Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]))
    for e in errs:
        print(f"[FAIL] {e}")
    sys.exit(1 if errs else 0)
```

- [ ] **Step 4: Add the smoke to `ci_validate.sh --native`**

At the end of the native-mode checks (after the KLV check, before the summary), add:

```bash
if [ "${MODE}" = "native" ]; then
    echo "==> Render smoke (primary view, /snapshot, frame stats)..."
    "${SCRIPT_DIR}/stop.sh" >/dev/null 2>&1 || true   # run_bench launches its own instance
    SMOKE_DIR="${WORK_DIR}/bench-smoke"
    if uv run --with numpy --with pillow python "${SCRIPT_DIR}/bench/run_bench.py" \
          --label ci-smoke --smoke --skip-warmup --out "${SMOKE_DIR}" >"${WORK_DIR}/bench.log" 2>&1; then
        CAP_W="$(grep -E '^capture_width:' "${REPO_ROOT}/deploy/camsim_config.yaml" | awk '{print $2}')"
        CAP_H="$(grep -E '^capture_height:' "${REPO_ROOT}/deploy/camsim_config.yaml" | awk '{print $2}')"
        uv run --with numpy --with pillow python "${SCRIPT_DIR}/bench/check_smoke.py" \
            "${SMOKE_DIR}" "${CAMSIM_CAPTURE_WIDTH:-${CAP_W}}" "${CAMSIM_CAPTURE_HEIGHT:-${CAP_H}}" \
            || fail "render smoke checks"
    else
        fail "run_bench.py --smoke (see ${WORK_DIR}/bench.log)"
        tail -20 "${WORK_DIR}/bench.log"
    fi
fi
```

Check how `ci_validate.sh` stops its own instance in `cleanup()`, so the smoke doesn't collide with the earlier instance or leave one behind. Check the real env var names for capture size in `docs/configuration.md`. Add `uv` to the script's "Requirements" comment.

- [ ] **Step 5: Run it**

Run: `scripts/ci_validate.sh --native`
Expected: all existing checks pass, then `Render smoke` passes; exit 0.

- [ ] **Step 6: Commit**

```bash
git add scripts/ci_validate.sh scripts/bench/check_smoke.py scripts/tests/test_bench_check_smoke.py
git commit -m "test(ci): render smoke in ci_validate --native (3A)"
```

---

### Task 11: Measure, then tune (hitches, SSE, LOD transition length, origin-shift distance)

Driven by data: each lever is its own commit with before/after `compare.py` output in the message. **Don't fix things the measurements don't show.**

**Files:** depend on findings. Expected touch points: `deploy/camsim_config.yaml`, `Config/CamSimConfig.h` defaults, `docs/configuration.md`, `Geospatial/` tileset tuning, `Config/DefaultEngine.ini`.

- [ ] **Step 1: After-run with defaults**

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label after-switch
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label after-switch-b --skip-warmup
uv run --with numpy --with pillow python scripts/bench/compare.py \
  <dir-of-baseline-b> .cache/bench/*-after-switch-b
```

For `<dir-of-baseline-b>`, use the Task 5 run directory. If it's been cleaned, make a directory with `results.json` copied from `scripts/bench/baselines/macos-m1pro-baseline.json` and `shots/` from `scripts/bench/shots/macos/baseline/`.

Save the table to the scratchpad; it's the starting point.

- [ ] **Step 2: Attribute hitches**

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label hitch-trace --skip-warmup --trace
python3 - <<'EOF'
import json, glob
d = sorted(glob.glob('.cache/bench/*-hitch-trace'))[-1]
rows = [json.loads(l) for l in open(f'{d}/frames.jsonl') if l.strip()]
for r in rows:
    if r['wall_ms'] > 66.7:
        print(f"t={r['t']:.2f} wall={r['wall_ms']:.0f} game={r['game_ms']:.0f} render={r['render_ms']:.0f} gpu={r['gpu_ms']:.0f} load={r['load_pct']:.0f}")
EOF
```

Open `trace.utrace` in Unreal Insights (`"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealInsights.app"`). For the worst five hitch frames, record which thread was long and the top timers under it. Classify each:

| Class | Evidence | Lever to try |
| ----- | -------- | ------------ |
| Tile physics cooking | Game thread long in Chaos/`CreatePhysicsMeshes`/`BodySetup` under Cesium tile load | Measure `CAMSIM_CREATE_PHYSICS_MESHES=0` (breaks HAT/HOT, LOS and KLV frame centre, so this only proves the cause). The real fix gets a design note in ROADMAP (e.g. cook async, or collision only for tiles near the boresight), not an ad-hoc change here. |
| glTF mesh creation | Game thread long in `UCesiumGltfComponent`/`CreateModelComponents` | Lower `max_simultaneous_tile_loads` / `loading_descendant_limit`; measure. |
| Shader/PSO compile | Render/RHI thread long in PSO creation on first sight of a material | `scripts/prewarm_shaders.sh`; enable PSO precaching (`r.PSOPrecaching=1`) in `DefaultEngine.ini`; measure. |
| Texture streaming | Render thread in texture streaming, pool over budget in logs | `performance.texture_pool_budget_mb`; measure. |
| GC | Game thread in `CollectGarbage` | `gc.TimeBetweenPurgingPendingKillObjects`, incremental GC; measure. |

For each lever you apply:
1. make the change;
2. `scripts/run.sh --build-only` if it's code;
3. run the bench with `--skip-warmup`, then `compare.py` against the previous step;
4. keep the change only if `hitches_66` or p99 improves beyond the run-to-run noise measured in Task 5;
5. commit with the table.

- [ ] **Step 3: SSE**

Starting from 16, try `CAMSIM_MAX_SSE` = 12, 8, 6, 4 with `--skip-warmup`. For each, record `wall_ms_p99`, `gpu_ms_p50`, `hitches_66` and `popin_fraction` per phase. Pick the lowest value whose p99 is ≤ the baseline's p99 in every phase. Set it as the default in `Config/CamSimConfig.h`, `deploy/camsim_config.yaml` (update the comment's history note) and `docs/configuration.md`. Commit with the sweep table.

- [ ] **Step 4: LOD transition length and origin-shift distance**

- Try `CAMSIM_LOD_TRANSITION_LENGTH` = 0.25, 0.5 and 1.0. Watch `slew.ts` (`ffplay`) for visible popping vs. blur, and check `popin_fraction`. Pick one.
- Try `CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M` = 10000, 20000 and 50000. Check the far-origin shots and the `far_origin` phase metrics. Pick the largest value whose far shots look as correct as the near ones. Set it as the default (non-zero from here on).

Commit each default change with its evidence.

- [ ] **Step 5: macOS dev profile if 1080p30 isn't reachable**

If `wall_ms_p95` > 33.3 ms in any phase at defaults, find the cheapest combination that gets there, in this order:
1. `rendering_quality.tsr_screen_percentage` (75, then 67);
2. SSE;
3. `r.Lumen.*` quality (`r.Lumen.DiffuseIndirect.Allow` scalability via `sg.GlobalIlluminationQuality=2`);
4. VSM (`rendering_quality.vsm_resolution_bias`).

Document it as "macOS dev profile" in `docs/configuration.md`: the env vars to set, the measured result, and what it costs visually (compare shots). If nothing reaches it, document the best achieved and the limiting thread/GPU time. Commit.

---

### Task 12: Results, docs, roadmap, review

**Files:**
- Create: `scripts/bench/baselines/macos-m1pro-3a.json`, `scripts/bench/shots/macos/3a/*.png`
- Modify: `ROADMAP.md`, `CLAUDE.md`, `docs/architecture.md`

- [ ] **Step 1: Final run and stored results**

```bash
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label 3a-final
uv run --with numpy --with pillow python scripts/bench/run_bench.py --label 3a-final-b --skip-warmup
cp .cache/bench/*-3a-final-b/results.json scripts/bench/baselines/macos-m1pro-3a.json
mkdir -p scripts/bench/shots/macos/3a && cp .cache/bench/*-3a-final-b/shots/*.png scripts/bench/shots/macos/3a/
```

- [ ] **Step 2: Check the exit criteria**

Using `compare.py <baseline-dir> <3a-final-b-dir>`, confirm each spec exit criterion and write the result next to it:
- `families_mean` = 1.0 in every phase. Also confirm once in the Insights trace from Task 11 that there's one scene render per frame.
- `orbit` `gpu_ms_p50` is lower than the baseline's.
- `hitches_66` in `orbit` and `low_pass` is at least 50% lower than the baseline's.
- `slew` `popin_fraction` is lower than the baseline's.
- Default SSE is ≤ 16, with p99 no worse than the baseline's.
- The macOS dev profile is documented (or the report explains what prevents it).
- `scripts/ci_validate.sh --native` passes. The full headless `RunTests CamSim` passes. `uv run --with pytest --with numpy --with pillow pytest scripts/tests/ -v` passes.

Any criterion that fails: say so plainly in ROADMAP, with the numbers. Don't mark 3A done.

- [ ] **Step 3: Update docs**

- `ROADMAP.md` Milestone 3:
  - mark 3A done (or partially done) with the key numbers (families 2→1, GPU ms, hitches, SSE, pop-in);
  - note the deferred 5090 runs and the physics-cooking design note if Task 11 found it;
  - under the ROADMAP's "editor changes for a human" list, add anything that needs the editor. For example, if tileset actors in `Main.umap` should be saved as Movable instead of being set at runtime.
- `CLAUDE.md`:
  - update the test count (`210 tests across 37 files`: recount with `grep -rh "IMPLEMENT_SIMPLE_AUTOMATION_TEST" unreal_project/CamSimTest/Source/CamSimTest/Tests | wc -l` and `ls .../Tests | wc -l`);
  - add `scripts/bench/run_bench.py` to the Commands table;
  - add a Gotcha: "The sensor is the primary view (`render.view_source`); `SceneCapture` only holds pose/FOV/post-process until 3B. Don't call `CaptureScene()` in primary mode";
  - add a Gotcha: "`bUseFixedFrameRate` makes `DeltaTime` constant; measure frame time with wall clock".
- `docs/architecture.md`: update the data-flow description (primary view → grab extension → readback) and the thread notes.

- [ ] **Step 4: Commit**

```bash
git add scripts/bench/baselines scripts/bench/shots ROADMAP.md CLAUDE.md docs/architecture.md
git commit -m "docs: ROADMAP 3A results on macOS (render path + measurement)"
```

- [ ] **Step 5: Ask the user to review the reference shots**

Show the user baseline vs. 3A shot pairs (`scripts/bench/shots/macos/baseline/` vs `.../3a/`), especially `far_origin_*`, plus the `slew.ts` from both runs. Their sign-off on the visuals is a spec exit criterion. Don't claim 3A done without it.
