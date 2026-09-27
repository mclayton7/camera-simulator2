# GPU Sensor Model 3B.1 (Pipeline) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace UE's tonemapper with an RDG compute sensor pipeline (HDR input → sensor AE/AGC → waveband and response curve → NV12) that streams through the existing encoder, selectable per session, with the legacy path unchanged.

**Architecture:** A new `CamSimShaders` module (loads at `PostConfigInit`) holds the compute shaders and `AddSensorPasses()`. `FCamSimFrameGrabExtension` subscribes to UE 5.8's `ReplacingTonemapper` pass and runs the graph every frame; the NV12 buffer goes into the existing readback ring, and a 256-bin log2 histogram goes to a small stats readback ring whose newest result lands in a mailbox. `FSensorController` (game thread, pure C++) turns histograms into next-frame gain/offset. `FSensorPathSelector` picks `Gpu` or `Legacy` per session; the legacy path (UE tonemapper + backbuffer grab + CPU model) is untouched.

**Tech Stack:** UE 5.8 C++ (RDG, global compute shaders, HLSL `.usf`), FFmpeg (libavcodec/libswscale already linked), UE Automation tests, Python 3 bench (`scripts/bench/`, pytest).

**Spec:** `docs/superpowers/specs/2026-09-27-gpu-sensor-model-design.md` (3B.1 row of "Effect staging"). 3B.2–3B.4 get their own plans.

## Global Constraints

- macOS (M1 Pro, Metal) is the only verified platform; nothing may be Metal-specific: plain compute only, **integer atomics only, no float atomics, no wave intrinsics**.
- Copyright header on every new C++/HLSL file: `// Copyright CamSim Contributors. All Rights Reserved.`
- UE naming (`F`/`U`/`A`/`E` prefixes, PascalCase, verb-first functions); `#include "CoreMinimal.h"` first, `.generated.h` last; forward declarations over includes in headers.
- Every new config key has a `CAMSIM_*` env override and is documented in `docs/configuration.md` (single source of truth) and `deploy/camsim_config.yaml`.
- Altitudes/time: sim time comes from `FSimClock::Get()` (UTC micros), never `DeltaTime`, for sensor state.
- Legacy behaviour must be byte-for-byte unchanged when the selected path is `Legacy` (the default config selects `Legacy` in 3B.1 because it enables effects not yet ported).
- NV12: capture width divisible by 4, height even; Y plane (W×H bytes) then interleaved UV (W×H/2 bytes), BT.709 **limited range** (Y 16–235, UV 16–240).
- Histogram: 256 bins, log2 range [−16, +16), 8 bins per stop, bin 0 also holds every signal ≤ 2⁻¹⁶ (including 0/NaN).
- Tolerances (GPU vs reference): Y ≤ 1 DN, U/V ≤ 2 DN, histogram bin-exact.
- Commit after every task; messages in the repo's style (`feat(sensor): …`, `test(sensor): …`) ending with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
  ```

### Commands used throughout

```bash
# Build (from repo root). Incremental ~10 s; adding a module or shader forces more.
set -o pipefail; scripts/run.sh --build-only 2>&1 | tail -5

# NullRHI automation tests (CI-equivalent). Replace the filter as needed.
UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
rm -rf .cache/automation-report
"$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim.Sensor+Quit" -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$PWD/.cache/automation-report" \
  -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput > .cache/automation.log 2>&1
python3 -c "
import json;d=json.load(open('.cache/automation-report/index.json',encoding='utf-8-sig'))
print('succeeded',d['succeeded'],'failed',d['failed'])
[print('FAIL',t['fullTestPath'],[e['event']['message'] for e in t['entries'] if e['event']['type']=='Error']) for t in d['tests'] if t['state']=='Fail']"

# GPU tests (real Metal RHI) — created in Task 5.
scripts/run_gpu_tests.sh            # runs CamSim.GPU.*
```

## Refinements to the spec (decided while planning — review these)

1. **UE exposure is manual but not constant.** A single fixed pre-exposure cannot hold both daylight (~2¹³) and night NVG (~2⁻¹²) scene values in fp16 scene colour. The controller also sets UE's manual `AutoExposureBias` each tick so scene colour stays well scaled, and the shader divides out the exact `View.OneOverPreExposure`. Gain is still applied to absolute scene-linear values.
2. **The histogram is accumulated inside the Apply pass** (no separate stats pass) and reaches the game thread through a mutex-protected mailbox fed by its own readback ring, not through the NV12 ring.
3. **`render.sensor_path: auto | gpu | legacy`** (env `CAMSIM_RENDER_SENSOR_PATH`, default `auto`). `auto` is the spec's selector; `gpu` forces the GPU path and logs the enabled-but-unported effects it ignores; `legacy` forces the old path. 3B.1's bench runs with `gpu`, because the default config enables effects that arrive in 3B.2/3B.3.
4. **The path is fixed for the session** (chosen at startup). The legacy path is deleted in 3B.4, so hot-reload switching is not built.
5. **`GET /snapshot/sensor`** (a second route) instead of `?stage=sensor`. In the GPU path of 3B.1, `/snapshot` returns the same frame as `/snapshot/sensor` (3B.1 has no effects to bypass); 3B.2 adds the effects-bypass scene image.
6. **Path reporting goes to `/metrics` and `camsim_health.json`.** `GET /health` is the liveness watchdog and stays as is.
7. **The encoder keeps YUV420P as the codec input.** NV12 is de-interleaved on the CPU (memcpy Y, split UV; no `sws_scale`). Codec-native NV12 / GPU textures are 3C.
8. **Exposure keys are `min_gain_ev` / `max_gain_ev`** (log2 of the gain applied to absolute scene-linear values; higher = brighter), which is unambiguous where the spec's `min_ev`/`max_ev` was not. The detector signal weights are per-mode keys `signal_weight_r/g/b`.
9. **NV12 digital zoom is nearest-neighbour**, matching today's BGRA `ApplyDigitalZoom`.

## Review Focus

1. **Scene luminance far outside the histogram range** (direct sun on water, a moonless night): the loop must clamp to `[min_gain_ev, max_gain_ev]` and never produce NaN gain. → Task 3 tests `EmptyHistogramKeepsGain`, `NightClampsAtMaxGain`, `SaturatedHistogramHitsMinGain`.
2. **Sim clock frozen or in lockstep (Δt = 0) or jumping backwards:** AE must hold, not divide by zero or snap. → Task 3 test `ZeroDeltaHolds`.
3. **Capture size not a multiple of 4, or odd height, with the GPU path selected:** must be a config validation error before any buffer is sized. → Task 1 test `Nv12DimensionsValidated`.
4. **The viewport is a different size from the capture** (window chrome, Retina): the NV12 must still be capture-sized and the "stretched" warning logged once. → Task 5 test `GpuMatchesReferenceScaled` (a 2× view resampled to the capture size, on a 2×2-constant scene so bilinear sampling is exact) and the Task 8 live check.
5. **The encoder falling behind while the GPU path runs:** AE must keep updating (stats ring is independent of the NV12 ring) and skipped frames must still count as `encoder_busy`. → Task 6 mailbox test `NewestWins` + Task 8 live check with `CAMSIM_ENCODER=libx264` at 1080p.

---

## File map

| File | Responsibility |
| --- | --- |
| `unreal_project/CamSimTest/CamSimTest.uproject` | add `CamSimShaders` module (`PostConfigInit`) |
| `unreal_project/CamSimTest/Source/CamSimTest.Target.cs`, `CamSimTestEditor.Target.cs` | `ExtraModuleNames.Add("CamSimShaders")` |
| `Source/CamSimShaders/CamSimShaders.Build.cs` | new module rules |
| `Source/CamSimShaders/Private/CamSimShadersModule.cpp` | module; maps `/CamSim` → `unreal_project/CamSimTest/Shaders` |
| `Source/CamSimShaders/Public/SensorFrameParams.h` | `FSensorFrameParams`, `FSensorHistogram`, histogram constants |
| `Source/CamSimShaders/Public/SensorGraph.h` / `Private/SensorGraph.cpp` | shader classes, `AddSensorPasses()` |
| `unreal_project/CamSimTest/Shaders/Private/CamSimSensor.usf` | `ApplyCS`, `PackNv12CS` |
| `Source/CamSimTest/Sensor/SensorController.{h,cpp}` | AE/AGC control loop |
| `Source/CamSimTest/Sensor/SensorReference.{h,cpp}` | scalar reference model |
| `Source/CamSimTest/Sensor/SensorPath.{h,cpp}` | `FSensorPathSelector` |
| `Source/CamSimTest/Sensor/SensorStatsMailbox.h` | newest-histogram hand-off (render → game) |
| `Source/CamSimTest/Encoder/Nv12.{h,cpp}` | NV12 ↔ BGRA / YUV420P helpers |
| `Source/CamSimTest/Encoder/IFrameSink.h`, `EncoderThread.{h,cpp}`, `VideoEncoder.{h,cpp}`, `MultiViewFrameSink.{h,cpp}` | `FSensorFrame` (BGRA8 or NV12) through the encoder |
| `Source/CamSimTest/Camera/SensorGpuTimer.{h,cpp}` | timestamp queries around the graph |
| `Source/CamSimTest/Camera/CamSimFrameGrabExtension.{h,cpp}` | `ReplacingTonemapper` subscription, NV12/stat readbacks |
| `Source/CamSimTest/Camera/CamSimCaptureComponent.{h,cpp}` | path selection, NV12 slots, controller, UE manual exposure |
| `Source/CamSimTest/Camera/CamSimCamera.cpp`, `CamSimFrameStats.{h,cpp}` | controller tick, new frame-stats keys |
| `Source/CamSimTest/Config/CamSimConfig.{h,cpp}`, `Sensor/SensorTypes.h` | `exposure`, signal weights, `render.sensor_path`, validation |
| `Source/CamSimTest/Health/CamSimHealthServer.{h,cpp}`, `Subsystem/CamSimSubsystem.cpp` | `/snapshot/sensor`, `sensor_path` reporting |
| `Source/CamSimTest/Tests/SensorControllerTest.cpp`, `SensorReferenceTest.cpp`, `SensorGpuTest.cpp`, `SensorPathTest.cpp`, `Nv12Test.cpp`, `EncoderNv12Test.cpp`, `SensorConfigTest.cpp` | tests |
| `scripts/run_gpu_tests.sh` | GPU automation runner |
| `scripts/bench/{run_bench,scenario,analyze}.py`, `scripts/tests/test_bench_*.py` | sensor timing, path assertion, sensor shots |
| `deploy/camsim_config.yaml`, `docs/configuration.md`, `ROADMAP.md`, `CLAUDE.md` | config and docs |

(`Source/…` means `unreal_project/CamSimTest/Source/…`.)

---

### Task 1: Config — exposure block, signal weights, `render.sensor_path`, NV12 validation

**Files:**
- Modify: `Source/CamSimTest/Sensor/SensorTypes.h` (add to `FSensorModeConfig`)
- Modify: `Source/CamSimTest/Config/CamSimConfig.h:801-829` (`FRenderConfig`), `Config/CamSimConfig.cpp` (`ParseMode` lambda ~l.558, `render` block ~l.1401, env overrides ~l.1822, `Validate()` ~l.1843)
- Modify: `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `Source/CamSimTest/Tests/SensorConfigTest.cpp` (new)

**Interfaces:**
- Produces: `FSensorExposureConfig` (in `SensorTypes.h`), `FSensorModeConfig::Exposure`, `FSensorModeConfig::SignalWeights` (`FVector3f`), `FCamSimConfig::FRenderConfig::ESensorPath { Auto, Gpu, Legacy }`, `FRenderConfig::SensorPath` (string) / `SensorPathMode` (enum).

- [ ] **Step 1: Write the failing tests** — `Tests/SensorConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorExposureYamlTest,
	"CamSim.Sensor.Config.ExposureFromYaml",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorExposureYamlTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n"
		"  nvg:\n"
		"    signal_weight_r: 0.6\n"
		"    signal_weight_g: 0.3\n"
		"    signal_weight_b: 0.1\n"
		"    exposure:\n"
		"      auto: false\n"
		"      min_gain_ev: -18\n"
		"      max_gain_ev: 7\n"
		"      target_grey: 0.3\n"
		"      highlight_percentile: 0.95\n"
		"      lag_frames: 4\n"
		"      manual_gain_ev: -2\n"
		"render:\n"
		"  sensor_path: gpu\n"));
	const FSensorModeConfig& N = Cfg.SensorModeConfigs.FindChecked(ESensorMode::NVG);
	TestEqual(TEXT("weight r"), N.SignalWeights.X, 0.6f);
	TestEqual(TEXT("weight b"), N.SignalWeights.Z, 0.1f);
	TestFalse(TEXT("auto"), N.Exposure.bAuto);
	TestEqual(TEXT("min"), N.Exposure.MinGainEv, -18.0f);
	TestEqual(TEXT("max"), N.Exposure.MaxGainEv, 7.0f);
	TestEqual(TEXT("grey"), N.Exposure.TargetGrey, 0.3f);
	TestEqual(TEXT("hi pct"), N.Exposure.HighlightPercentile, 0.95f);
	TestEqual(TEXT("lag"), N.Exposure.LagFrames, 4);
	TestEqual(TEXT("manual"), N.Exposure.ManualGainEv, -2.0f);
	TestTrue(TEXT("sensor_path gpu"), Cfg.Render.SensorPathMode == FCamSimConfig::FRenderConfig::ESensorPath::Gpu);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorExposureDefaultsTest,
	"CamSim.Sensor.Config.ExposureDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorExposureDefaultsTest::RunTest(const FString& Parameters)
{
	const FSensorModeConfig Def;
	TestTrue(TEXT("auto by default"), Def.Exposure.bAuto);
	TestEqual(TEXT("BT.709 luminance weights sum to 1"),
		Def.SignalWeights.X + Def.SignalWeights.Y + Def.SignalWeights.Z, 1.0f, 1e-5f);
	const FCamSimConfig Cfg;
	TestTrue(TEXT("sensor_path auto"), Cfg.Render.SensorPathMode == FCamSimConfig::FRenderConfig::ESensorPath::Auto);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorNv12DimsTest,
	"CamSim.Sensor.Config.Nv12DimensionsValidated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorNv12DimsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.CaptureWidth = 1282;   // even, but not a multiple of 4
	Cfg.CaptureHeight = 720;
	auto HasError = [](const TArray<FString>& Errors, const TCHAR* Needle)
	{
		return Errors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	};
	TestTrue(TEXT("width % 4 reported"), HasError(Cfg.Validate(), TEXT("multiple of 4")));
	Cfg.CaptureWidth = 1280;
	TestFalse(TEXT("1280 accepted"), HasError(Cfg.Validate(), TEXT("multiple of 4")));

	FSensorModeConfig& Eo = Cfg.SensorModeConfigs.FindOrAdd(ESensorMode::EO);
	Eo.Exposure.MinGainEv = 3.0f;
	Eo.Exposure.MaxGainEv = 2.0f;
	TestTrue(TEXT("min > max reported"), HasError(Cfg.Validate(), TEXT("min_gain_ev")));
	Eo.Exposure.MinGainEv = -20.0f;
	Eo.Exposure.HighlightPercentile = 1.5f;
	TestTrue(TEXT("percentile range reported"), HasError(Cfg.Validate(), TEXT("highlight_percentile")));
	Eo.Exposure.HighlightPercentile = 0.99f;
	Eo.AGCLowPercentile = 0.9f;
	Eo.AGCHighPercentile = 0.1f;
	TestTrue(TEXT("agc low >= high reported"), HasError(Cfg.Validate(), TEXT("agc_low_percentile")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPathEnvTest,
	"CamSim.Sensor.Config.SensorPathEnvOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorPathEnvTest::RunTest(const FString& Parameters)
{
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_RENDER_SENSOR_PATH"), TEXT("legacy"));
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("render:\n  sensor_path: gpu\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_RENDER_SENSOR_PATH"), TEXT(""));
	TestTrue(TEXT("env wins"), Cfg.Render.SensorPathMode == FCamSimConfig::FRenderConfig::ESensorPath::Legacy);
	return true;
}
```

- [ ] **Step 2: Build; the test file must fail to compile** (`Exposure`, `SignalWeights`, `SensorPathMode` don't exist). Run the build command. Expected: compile errors naming those members.

- [ ] **Step 3: Add the config types.** In `Sensor/SensorTypes.h`, above `struct FSensorModeConfig`:

```cpp
// ---------------------------------------------------------------------------
// FSensorExposureConfig — sensor auto-exposure (ROADMAP 3B). Gains are log2 of
// the multiplier applied to absolute scene-linear values: higher = brighter.
// ---------------------------------------------------------------------------
struct FSensorExposureConfig
{
	/** Auto-exposure on; false uses ManualGainEv. */
	bool  bAuto               = true;
	/** Camera limits: shortest integration / lowest gain … highest gain. */
	float MinGainEv           = -20.0f;
	float MaxGainEv           = -6.0f;
	/** Linear value the histogram median is exposed to. */
	float TargetGrey          = 0.18f;
	/** This percentile of the histogram is kept below clipping. */
	float HighlightPercentile = 0.99f;
	/** Convergence time constant in frames at 30 Hz (sim time); 0 = instant. */
	int32 LagFrames           = 2;
	float ManualGainEv        = -12.0f;
};
```

and inside `FSensorModeConfig` (after `BrightnessBias`):

```cpp
	/** Detector spectral response: signal = dot(scene RGB, SignalWeights). Default BT.709 luminance. */
	FVector3f SignalWeights = FVector3f(0.2126f, 0.7152f, 0.0722f);

	/** Sensor auto-exposure (GPU sensor path, ROADMAP 3B). */
	FSensorExposureConfig Exposure;
```

`SensorTypes.h` needs `#include "Math/Vector.h"` only if `FVector3f` is not already visible through `CoreMinimal.h` (it is).

In `CamSimConfig.h`, inside `FRenderConfig` after `ExposureCompensationEV`:

```cpp
		enum class ESensorPath : uint8
		{
			Auto = 0,  // GPU when every enabled effect is ported, else legacy (ROADMAP 3B)
			Gpu,       // force the GPU sensor model; unported effects are ignored (logged)
			Legacy,    // force UE tonemapper + CPU sensor model
		};
		// auto | gpu | legacy. Env: CAMSIM_RENDER_SENSOR_PATH
		FString     SensorPath     = TEXT("auto");
		ESensorPath SensorPathMode = ESensorPath::Auto;
```

- [ ] **Step 4: Parse them.** In `CamSimConfig.cpp`, next to `ParseViewSource`, add:

```cpp
static FCamSimConfig::FRenderConfig::ESensorPath ParseSensorPath(const FString& S)
{
	using E = FCamSimConfig::FRenderConfig::ESensorPath;
	if (S.Equals(TEXT("gpu"), ESearchCase::IgnoreCase))    return E::Gpu;
	if (S.Equals(TEXT("legacy"), ESearchCase::IgnoreCase)) return E::Legacy;
	if (!S.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogCamSim, Warning, TEXT("Config: render.sensor_path '%s' is not auto|gpu|legacy; using auto"), *S);
	}
	return E::Auto;
}
```

In the `ParseMode` lambda, after the `sun_glint_spread` line:

```cpp
				YamlFloat(ModeNode, "signal_weight_r", MC.SignalWeights.X);
				YamlFloat(ModeNode, "signal_weight_g", MC.SignalWeights.Y);
				YamlFloat(ModeNode, "signal_weight_b", MC.SignalWeights.Z);
				if (YamlHas(ModeNode, "exposure"))
				{
					ryml::ConstNodeRef ENode = ModeNode["exposure"];
					YamlBool (ENode, "auto",                 MC.Exposure.bAuto);
					YamlFloat(ENode, "min_gain_ev",          MC.Exposure.MinGainEv);
					YamlFloat(ENode, "max_gain_ev",          MC.Exposure.MaxGainEv);
					YamlFloat(ENode, "target_grey",          MC.Exposure.TargetGrey);
					YamlFloat(ENode, "highlight_percentile", MC.Exposure.HighlightPercentile);
					YamlInt  (ENode, "lag_frames",           MC.Exposure.LagFrames);
					YamlFloat(ENode, "manual_gain_ev",       MC.Exposure.ManualGainEv);
				}
```

In the `render` block after `exposure_compensation_ev`:

```cpp
			YamlString(RNode, "sensor_path",             Cfg.Render.SensorPath);
```

and after the `render` block closes (so an env override is re-parsed too) — in the env-override function next to `CAMSIM_RENDER_EXPOSURE_COMPENSATION_EV`:

```cpp
	Cfg.Render.SensorPath     = GetEnvString(TEXT("CAMSIM_RENDER_SENSOR_PATH"), Cfg.Render.SensorPath);
	Cfg.Render.SensorPathMode = ParseSensorPath(Cfg.Render.SensorPath);
```

Use whatever the file's existing string env helper is called (search the file for `CAMSIM_RENDER_VIEW_SOURCE` and copy that line's helper). An empty env value must mean "unset" — check that helper's behaviour; the test sets it to empty afterwards.

- [ ] **Step 5: Validate.** In `Validate()` after the even-dimension checks:

```cpp
	// NV12 packing writes 4 bytes per uint (ROADMAP 3B)
	if (CaptureWidth % 4 != 0)
	{
		Errors.Add(FString::Printf(TEXT("CaptureWidth=%d must be a multiple of 4 (NV12 packing)"), CaptureWidth));
	}
	for (const TPair<ESensorMode, FSensorModeConfig>& Pair : SensorModeConfigs)
	{
		const FSensorModeConfig& M = Pair.Value;
		const int32 ModeId = static_cast<int32>(Pair.Key);
		if (M.Exposure.MinGainEv > M.Exposure.MaxGainEv)
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].exposure: min_gain_ev (%.1f) > max_gain_ev (%.1f)"),
				ModeId, M.Exposure.MinGainEv, M.Exposure.MaxGainEv));
		}
		if (M.Exposure.HighlightPercentile <= 0.0f || M.Exposure.HighlightPercentile > 1.0f)
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].exposure: highlight_percentile=%.3f out of (0, 1]"),
				ModeId, M.Exposure.HighlightPercentile));
		}
		if (M.AGCLowPercentile < 0.0f || M.AGCHighPercentile > 1.0f || M.AGCLowPercentile >= M.AGCHighPercentile)
		{
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d]: agc_low_percentile (%.3f) / agc_high_percentile (%.3f) must satisfy 0 <= low < high <= 1"),
				ModeId, M.AGCLowPercentile, M.AGCHighPercentile));
		}
	}
```

- [ ] **Step 6: Canonical config and docs.** In `deploy/camsim_config.yaml`: add to `render:` `sensor_path: auto  # auto | gpu | legacy (ROADMAP 3B)`; add to each of `sensor_modes.eo/ir/nvg` an `exposure:` block with the defaults (EO/IR: `min_gain_ev: -20`, `max_gain_ev: -6`, `target_grey: 0.18`; NVG: `max_gain_ev: 6`, `target_grey: 0.3`, `signal_weight_r: 0.6`, `signal_weight_g: 0.3`, `signal_weight_b: 0.1`), `highlight_percentile: 0.99`, `lag_frames: 2`, `manual_gain_ev: -12`, each commented "provisional; calibrated in Task 11". In `docs/configuration.md` add rows for every new key with env var (`CAMSIM_RENDER_SENSOR_PATH`), default and "ROADMAP 3B" annotation; per-mode keys have no env overrides — say so, matching how the existing `sensor_modes` rows are documented.

- [ ] **Step 7: Build and run** `CamSim.Sensor.Config` and `CamSim.Config` (the canonical-config test `Config.CanonicalConfigHasNoUnknownKeys` must still pass). Expected: all pass.

- [ ] **Step 8: Commit** — `feat(config): sensor exposure block, signal weights and render.sensor_path (3B.1)`.

---

### Task 2: `CamSimShaders` module and the shared parameter types

**Files:**
- Create: `Source/CamSimShaders/CamSimShaders.Build.cs`, `Source/CamSimShaders/Private/CamSimShadersModule.cpp`, `Source/CamSimShaders/Public/SensorFrameParams.h`
- Create: `unreal_project/CamSimTest/Shaders/Private/CamSimSensor.usf` (placeholder kernel, filled in Task 5)
- Modify: `unreal_project/CamSimTest/CamSimTest.uproject`, `Source/CamSimTest.Target.cs`, `Source/CamSimTestEditor.Target.cs`, `Source/CamSimTest/CamSimTest.Build.cs`
- Test: `Source/CamSimTest/Tests/SensorPathTest.cpp` (new; module test only in this task)

**Interfaces:**
- Produces (`SensorFrameParams.h`, namespace-free, header-only):

```cpp
enum class ESensorGraphMode : uint32 { EO = 0, IR = 1, NVG = 2 };

struct FSensorHistogram
{
	static constexpr int32 NumBins     = 256;
	static constexpr float MinLog2     = -16.0f;
	static constexpr float BinsPerStop = 8.0f;
	TStaticArray<uint32, NumBins> Bins;
	uint32 Serial = 0;   // FSensorFrameParams::Serial of the frame it measured
	FSensorHistogram() { for (uint32& B : Bins) B = 0; }
	uint64 Total() const { uint64 T = 0; for (uint32 B : Bins) T += B; return T; }
	static float BinCentreLog2(int32 Bin) { return MinLog2 + (Bin + 0.5f) / BinsPerStop; }
	/** Histogram bin of a detector signal; <= 2^MinLog2, 0 and NaN land in bin 0. */
	static int32 BinOf(float Signal)
	{
		if (!(Signal > 0.0f)) return 0;
		const int32 B = FMath::FloorToInt32((FMath::Log2(Signal) - MinLog2) * BinsPerStop);
		return FMath::Clamp(B, 0, NumBins - 1);
	}
};

/** Everything one frame of the sensor graph needs besides its input textures. */
struct FSensorFrameParams
{
	ESensorGraphMode Mode = ESensorGraphMode::EO;
	uint32    bBlackHot     = 0;          // IR/NVG polarity: 1 inverts after gain/offset
	float     Gain          = 1.0f;       // linear multiplier on absolute scene-linear signal
	float     Offset        = 0.0f;       // added after Gain (IR AGC), normalised units
	FVector3f SignalWeights = FVector3f(0.2126f, 0.7152f, 0.0722f);
	float     KneeStart     = 0.8f;       // EO soft highlight knee (linear)
	FVector3f DisplayTint   = FVector3f(1.0f, 1.0f, 1.0f);  // viewport only (NVG green)
	float     InputScale    = 1.0f;       // multiplies scene colour; tests only (runtime uses View.OneOverPreExposure)
	uint32    Serial        = 0;          // tags the histogram this frame produces
};
```

- [ ] **Step 1: Write the failing test** — `Tests/SensorPathTest.cpp` (more tests join it in Task 6):

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"
#include "SensorFrameParams.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorShaderModuleTest,
	"CamSim.Sensor.ShaderModule.LoadedWithShaderDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorShaderModuleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("CamSimShaders loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("CamSimShaders")));
	TestTrue(TEXT("/CamSim shader directory mapped"), AllShaderSourceDirectoryMappings().Contains(TEXT("/CamSim")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorHistogramBinTest,
	"CamSim.Sensor.Histogram.BinOf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorHistogramBinTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("zero"), FSensorHistogram::BinOf(0.0f), 0);
	TestEqual(TEXT("negative"), FSensorHistogram::BinOf(-1.0f), 0);
	TestEqual(TEXT("NaN"), FSensorHistogram::BinOf(NAN), 0);
	TestEqual(TEXT("1.0 is stop 16"), FSensorHistogram::BinOf(1.0f), 128);
	TestEqual(TEXT("huge clamps"), FSensorHistogram::BinOf(1e30f), 255);
	for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
	{
		TestEqual(FString::Printf(TEXT("centre of bin %d"), B),
			FSensorHistogram::BinOf(FMath::Exp2(FSensorHistogram::BinCentreLog2(B))), B);
	}
	return true;
}
```

- [ ] **Step 2: Build** — expected: fails (`SensorFrameParams.h` not found).

- [ ] **Step 3: Create the module.** `Source/CamSimShaders/CamSimShaders.Build.cs`:

```csharp
// Copyright CamSim Contributors. All Rights Reserved.

using UnrealBuildTool;

public class CamSimShaders : ModuleRules
{
	public CamSimShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "Engine", "RenderCore", "RHI", "Renderer" });
	}
}
```

`Source/CamSimShaders/Private/CamSimShadersModule.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

/**
 * Global compute shaders for the GPU sensor model (ROADMAP 3B). Loads at
 * PostConfigInit, before the global shader map is compiled, and maps the
 * virtual directory /CamSim to <project>/Shaders.
 */
class FCamSimShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders")));
		if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/CamSim")))
		{
			AddShaderSourceDirectoryMapping(TEXT("/CamSim"), Dir);
		}
	}
};

IMPLEMENT_MODULE(FCamSimShadersModule, CamSimShaders)
```

`Source/CamSimShaders/Public/SensorFrameParams.h`: the copyright header, `#pragma once`, `#include "CoreMinimal.h"`, `#include "Containers/StaticArray.h"`, then exactly the types in **Interfaces** above.

`unreal_project/CamSimTest/Shaders/Private/CamSimSensor.usf` (placeholder so the mapping points at a real directory):

```hlsl
// Copyright CamSim Contributors. All Rights Reserved.
// GPU sensor model (ROADMAP 3B). Kernels are added in Task 5.
#include "/Engine/Public/Platform.ush"
```

- [ ] **Step 4: Register it.** `CamSimTest.uproject` `Modules` array gains `{ "Name": "CamSimShaders", "Type": "Runtime", "LoadingPhase": "PostConfigInit" }` (before `CamSimTest`). Both `*.Target.cs`: `ExtraModuleNames.Add("CamSimShaders");` next to the existing line. `CamSimTest.Build.cs`: add `"CamSimShaders"` to `PublicDependencyModuleNames`.

- [ ] **Step 5: Build and run** `CamSim.Sensor.ShaderModule` and `CamSim.Sensor.Histogram` under NullRHI. Expected: both pass. (If UBT complains about the new target module, re-run `scripts/run.sh --build-only`; a new module changes the makefile.)

- [ ] **Step 6: Commit** — `feat(sensor): CamSimShaders module and shared sensor parameter types (3B.1)`.

---

### Task 3: `FSensorController` — AE/AGC control loop

**Files:**
- Create: `Source/CamSimTest/Sensor/SensorController.h`, `Source/CamSimTest/Sensor/SensorController.cpp`
- Test: `Source/CamSimTest/Tests/SensorControllerTest.cpp`

**Interfaces:**
- Consumes: `FSensorHistogram`, `FSensorFrameParams`, `ESensorGraphMode` (Task 2); `FSensorModeConfig`, `FSensorExposureConfig` (Task 1).
- Produces:

```cpp
struct FSensorControllerInput
{
	ESensorGraphMode        Mode        = ESensorGraphMode::EO;
	bool                    bBlackHot   = false;
	bool                    bCameraCut  = false;
	double                  DeltaSimSec = 0.0;
	const FSensorHistogram* NewHistogram = nullptr;  // newest delivered this tick, or null
	float                   ExposureCompensationEv = 0.0f;  // EO only
	uint32                  Serial      = 0;          // this tick's params serial (monotonic)
};

class FSensorController
{
public:
	static constexpr int32 StaleAfterTicks = 10;
	static constexpr float ClipLinear      = 2.0f;  // the knee maps 2.0 to ~255/255
	FSensorFrameParams Update(const FSensorControllerInput& In, const FSensorModeConfig& Cfg);
	float  GetGainEv() const          { return GainEv; }
	float  GetLastMedianLog2() const  { return LastMedianLog2; }
	uint32 GetStaleEpisodes() const   { return StaleEpisodes; }
	/** Percentile P in [0,1] of H as log2 signal; false for an empty histogram. */
	static bool PercentileLog2(const FSensorHistogram& H, float P, float& OutLog2);
private: /* see Step 3 */
};
```

- [ ] **Step 1: Write the failing tests** — `Tests/SensorControllerTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Sensor/SensorController.h"

namespace
{
	/** Every pixel at one log2 level (bin centre). */
	FSensorHistogram Flat(float Log2, uint32 Serial, uint32 Count = 10000)
	{
		FSensorHistogram H;
		H.Bins[FSensorHistogram::BinOf(FMath::Exp2(Log2))] = Count;
		H.Serial = Serial;
		return H;
	}

	FSensorModeConfig EoCfg()
	{
		FSensorModeConfig C;
		C.Exposure.MinGainEv = -20.0f;
		C.Exposure.MaxGainEv = -6.0f;
		C.Exposure.TargetGrey = 0.18f;
		C.Exposure.LagFrames = 0;
		return C;
	}

	FSensorControllerInput In(const FSensorHistogram* H, uint32 Serial, double Dt = 1.0 / 30.0)
	{
		FSensorControllerInput I;
		I.NewHistogram = H;
		I.Serial = Serial;
		I.DeltaSimSec = Dt;
		return I;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeMedianTest, "CamSim.Sensor.Controller.MedianToMidGrey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeMedianTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	const float SceneLog2 = FSensorHistogram::BinCentreLog2(FSensorHistogram::BinOf(FMath::Exp2(12.0f)));
	const FSensorHistogram H = Flat(12.0f, 1);
	const FSensorFrameParams P = C.Update(In(&H, 1), EoCfg());
	TestEqual(TEXT("median exposed to 0.18"), FMath::Exp2(SceneLog2) * P.Gain, 0.18f, 0.18f * 0.01f);
	TestEqual(TEXT("offset"), P.Offset, 0.0f);
	TestEqual(TEXT("median reported"), C.GetLastMedianLog2(), SceneLog2, 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeNightTest, "CamSim.Sensor.Controller.NightClampsAtMaxGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeNightTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	const FSensorHistogram H = Flat(-8.0f, 1);  // ~0.004: night
	const FSensorFrameParams P = C.Update(In(&H, 1), EoCfg());
	TestEqual(TEXT("gain at the camera limit"), C.GetGainEv(), -6.0f, 1e-4f);
	TestTrue(TEXT("scene stays dark (< 1% of full scale)"), FMath::Exp2(-8.0f) * P.Gain < 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeSaturatedTest, "CamSim.Sensor.Controller.SaturatedHistogramHitsMinGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeSaturatedTest::RunTest(const FString& Parameters)
{
	// Everything at/above 2^16 wants gain ~2^-18.4; a camera whose shortest
	// integration only reaches 2^-16 must stop there.
	FSensorController C;
	FSensorHistogram H;
	H.Bins[FSensorHistogram::NumBins - 1] = 1000;
	H.Serial = 1;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.MinGainEv = -16.0f;
	C.Update(In(&H, 1), Cfg);
	TestEqual(TEXT("gain at the lower limit"), C.GetGainEv(), -16.0f, 1e-4f);
	TestTrue(TEXT("finite"), FMath::IsFinite(C.GetGainEv()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeHighlightTest, "CamSim.Sensor.Controller.HighlightCapPreventsClipping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeHighlightTest::RunTest(const FString& Parameters)
{
	// 90% at 2^8, 10% at 2^14: exposing the median to 0.18 would put the
	// bright 10% at 0.18 * 64 = 11.5, far past ClipLinear.
	FSensorController C;
	FSensorHistogram H;
	H.Bins[FSensorHistogram::BinOf(FMath::Exp2(8.0f))]  = 9000;
	H.Bins[FSensorHistogram::BinOf(FMath::Exp2(14.0f))] = 1000;
	H.Serial = 1;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.HighlightPercentile = 0.95f;
	const FSensorFrameParams P = C.Update(In(&H, 1), Cfg);
	const float Hi = FMath::Exp2(FSensorHistogram::BinCentreLog2(FSensorHistogram::BinOf(FMath::Exp2(14.0f))));
	TestTrue(TEXT("95th percentile not clipped"), Hi * P.Gain <= FSensorController::ClipLinear * 1.0001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeLagTest, "CamSim.Sensor.Controller.LagIsFrameRateIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeLagTest::RunTest(const FString& Parameters)
{
	// tau = 3 frames at 30 Hz = 0.1 s. After 0.1 s of sim time, 63% of a step.
	for (const double Hz : { 30.0, 60.0 })
	{
		FSensorController C;
		FSensorModeConfig Cfg = EoCfg();
		Cfg.Exposure.LagFrames = 3;
		const FSensorHistogram A = Flat(12.0f, 1);
		C.Update(In(&A, 1), Cfg);                 // first frame snaps
		const float Start = C.GetGainEv();
		const FSensorHistogram B = Flat(10.0f, 2); // 2 stops darker: target +2 EV
		const int32 Steps = FMath::RoundToInt32(0.1 * Hz);
		for (int32 I = 0; I < Steps; ++I)
		{
			const FSensorHistogram Bi = Flat(10.0f, 2 + I);
			C.Update(In(&Bi, 2 + I, 1.0 / Hz), Cfg);
		}
		const float Frac = (C.GetGainEv() - Start) / 2.0f;
		TestEqual(FString::Printf(TEXT("63%% at tau (%.0f Hz)"), Hz), Frac, 1.0f - FMath::Exp(-1.0f), 0.02f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeSnapTest, "CamSim.Sensor.Controller.CutAndModeSwitchSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeSnapTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.LagFrames = 30;  // slow: only a snap reaches the target in one step
	const FSensorHistogram A = Flat(12.0f, 1);
	C.Update(In(&A, 1), Cfg);
	const float Day = C.GetGainEv();

	// Cut at serial 5. A histogram measured before the cut (serial 4) must not snap.
	FSensorControllerInput Cut = In(nullptr, 5);
	Cut.bCameraCut = true;
	C.Update(Cut, Cfg);
	const FSensorHistogram Old = Flat(10.0f, 4);
	C.Update(In(&Old, 6), Cfg);
	TestTrue(TEXT("pre-cut histogram eases"), FMath::Abs(C.GetGainEv() - (Day + 2.0f)) > 1.0f);
	const FSensorHistogram New = Flat(10.0f, 5);
	C.Update(In(&New, 7), Cfg);
	TestEqual(TEXT("post-cut histogram snaps"), C.GetGainEv(), Day + 2.0f, 0.01f);

	// Mode switch behaves like a cut.
	FSensorControllerInput Nvg = In(nullptr, 8);
	Nvg.Mode = ESensorGraphMode::NVG;
	C.Update(Nvg, Cfg);
	const FSensorHistogram N = Flat(8.0f, 8);
	FSensorControllerInput NvgH = In(&N, 9);
	NvgH.Mode = ESensorGraphMode::NVG;
	C.Update(NvgH, Cfg);
	TestEqual(TEXT("mode switch snaps"), C.GetGainEv(), Day + 4.0f, 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorIrAgcTest, "CamSim.Sensor.Controller.IrPercentileStretch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorIrAgcTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.bAGCEnabled = true;
	Cfg.AGCLowPercentile = 0.01f;
	Cfg.AGCHighPercentile = 0.99f;
	Cfg.AGCLagFrames = 0;
	FSensorHistogram H;
	const int32 LoBin = FSensorHistogram::BinOf(FMath::Exp2(4.0f));
	const int32 HiBin = FSensorHistogram::BinOf(FMath::Exp2(6.0f));
	H.Bins[LoBin] = 500; H.Bins[HiBin] = 500; H.Serial = 1;
	FSensorControllerInput I = In(&H, 1);
	I.Mode = ESensorGraphMode::IR;
	const FSensorFrameParams P = C.Update(I, Cfg);
	const float Lo = FMath::Exp2(FSensorHistogram::BinCentreLog2(LoBin));
	const float Hi = FMath::Exp2(FSensorHistogram::BinCentreLog2(HiBin));
	TestEqual(TEXT("low percentile -> 0"), Lo * P.Gain + P.Offset, 0.0f, 1e-4f);
	TestEqual(TEXT("high percentile -> 1"), Hi * P.Gain + P.Offset, 1.0f, 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorManualTest, "CamSim.Sensor.Controller.ManualGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorManualTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.bAuto = false;
	Cfg.Exposure.ManualGainEv = -9.0f;
	const FSensorHistogram H = Flat(12.0f, 1);
	const FSensorFrameParams P = C.Update(In(&H, 1), Cfg);
	TestEqual(TEXT("manual gain"), P.Gain, FMath::Exp2(-9.0f), 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorZeroDtTest, "CamSim.Sensor.Controller.ZeroDeltaHolds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorZeroDtTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.LagFrames = 3;
	const FSensorHistogram A = Flat(12.0f, 1);
	C.Update(In(&A, 1), Cfg);
	const float Before = C.GetGainEv();
	const FSensorHistogram B = Flat(8.0f, 2);
	C.Update(In(&B, 2, 0.0), Cfg);    // frozen clock
	TestEqual(TEXT("frozen: no change"), C.GetGainEv(), Before, 1e-6f);
	C.Update(In(&B, 3, -1.0), Cfg);   // backwards jump
	TestEqual(TEXT("negative dt: no change"), C.GetGainEv(), Before, 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorStaleTest, "CamSim.Sensor.Controller.StaleHistogramHolds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorStaleTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	const FSensorHistogram A = Flat(12.0f, 1);
	const FSensorFrameParams First = C.Update(In(&A, 1), EoCfg());
	FSensorFrameParams Last;
	for (int32 I = 0; I < 25; ++I) Last = C.Update(In(nullptr, 2 + I), EoCfg());
	TestEqual(TEXT("gain held"), Last.Gain, First.Gain);
	TestEqual(TEXT("one stale episode"), C.GetStaleEpisodes(), 1u);
	FSensorHistogram Empty; Empty.Serial = 30;
	C.Update(In(&Empty, 30), EoCfg());
	TestEqual(TEXT("empty histogram keeps gain"), C.GetGainEv(), FMath::Log2(First.Gain), 1e-5f);
	return true;
}
```

- [ ] **Step 2: Build** — expected: fails (`Sensor/SensorController.h` not found).

- [ ] **Step 3: Implement.** `Sensor/SensorController.h`: copyright, `#pragma once`, `CoreMinimal.h`, `SensorFrameParams.h`, `Sensor/SensorTypes.h`, then the **Interfaces** declarations plus private state:

```cpp
private:
	void UpdateAe(const FSensorHistogram& H, const FSensorModeConfig& Cfg, const FSensorControllerInput& In, bool bSnap);
	void UpdateIrAgc(const FSensorHistogram& H, const FSensorModeConfig& Cfg, const FSensorControllerInput& In, bool bSnap);
	static float Smoothing(double DeltaSimSec, int32 LagFrames, bool bSnap);

	bool   bInitialized    = false;
	bool   bSnapPending    = false;
	uint32 SnapAfterSerial = 0;
	ESensorGraphMode LastMode = ESensorGraphMode::EO;
	float  GainEv          = -12.0f;
	float  IrLoLog2        = 0.0f;
	float  IrHiLog2        = 1.0f;
	float  LastMedianLog2  = 0.0f;
	int32  TicksSinceHistogram = 0;
	uint32 StaleEpisodes   = 0;
```

`Sensor/SensorController.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorController.h"

bool FSensorController::PercentileLog2(const FSensorHistogram& H, float P, float& OutLog2)
{
	const uint64 Total = H.Total();
	if (Total == 0) return false;
	const double Target = FMath::Clamp(static_cast<double>(P), 0.0, 1.0) * static_cast<double>(Total);
	uint64 Cum = 0;
	for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
	{
		Cum += H.Bins[B];
		if (static_cast<double>(Cum) >= Target && H.Bins[B] > 0)
		{
			OutLog2 = FSensorHistogram::BinCentreLog2(B);
			return true;
		}
	}
	OutLog2 = FSensorHistogram::BinCentreLog2(FSensorHistogram::NumBins - 1);
	return true;
}

float FSensorController::Smoothing(double DeltaSimSec, int32 LagFrames, bool bSnap)
{
	if (bSnap || LagFrames <= 0) return 1.0f;
	if (!(DeltaSimSec > 0.0)) return 0.0f;                  // frozen or backwards: hold
	const double Tau = static_cast<double>(LagFrames) / 30.0;
	return static_cast<float>(1.0 - FMath::Exp(-DeltaSimSec / Tau));
}

void FSensorController::UpdateAe(const FSensorHistogram& H, const FSensorModeConfig& Cfg,
	const FSensorControllerInput& In, bool bSnap)
{
	const FSensorExposureConfig& E = Cfg.Exposure;
	float Target = E.ManualGainEv;
	float Median = 0.0f, High = 0.0f;
	if (PercentileLog2(H, 0.5f, Median) && PercentileLog2(H, E.HighlightPercentile, High))
	{
		LastMedianLog2 = Median;
		if (E.bAuto)
		{
			const float Comp = (In.Mode == ESensorGraphMode::EO) ? In.ExposureCompensationEv : 0.0f;
			const float ToGrey = FMath::Log2(FMath::Max(E.TargetGrey, 1e-6f)) + Comp - Median;
			const float NoClip = FMath::Log2(ClipLinear) - High;
			Target = FMath::Min(ToGrey, NoClip);
		}
	}
	else if (E.bAuto)
	{
		return;  // empty histogram: keep the current gain
	}
	Target = FMath::Clamp(Target, E.MinGainEv, E.MaxGainEv);
	GainEv += (Target - GainEv) * Smoothing(In.DeltaSimSec, E.LagFrames, bSnap);
}

void FSensorController::UpdateIrAgc(const FSensorHistogram& H, const FSensorModeConfig& Cfg,
	const FSensorControllerInput& In, bool bSnap)
{
	float Lo = 0.0f, Hi = 0.0f, Median = 0.0f;
	if (!PercentileLog2(H, Cfg.AGCLowPercentile, Lo) || !PercentileLog2(H, Cfg.AGCHighPercentile, Hi)) return;
	if (PercentileLog2(H, 0.5f, Median)) LastMedianLog2 = Median;
	Hi = FMath::Max(Hi, Lo + 1.0f / FSensorHistogram::BinsPerStop);  // never a zero-width band
	const float A = Smoothing(In.DeltaSimSec, Cfg.AGCLagFrames, bSnap);
	IrLoLog2 += (Lo - IrLoLog2) * A;
	IrHiLog2 += (Hi - IrHiLog2) * A;
}

FSensorFrameParams FSensorController::Update(const FSensorControllerInput& In, const FSensorModeConfig& Cfg)
{
	if (In.Mode != LastMode || In.bCameraCut)
	{
		bSnapPending = true;
		SnapAfterSerial = In.Serial;
		LastMode = In.Mode;
	}
	const bool bIrAgc = (In.Mode == ESensorGraphMode::IR) && Cfg.bAGCEnabled;

	if (In.NewHistogram)
	{
		TicksSinceHistogram = 0;
		const bool bSnap = !bInitialized || (bSnapPending && In.NewHistogram->Serial >= SnapAfterSerial);
		if (In.NewHistogram->Total() > 0)
		{
			if (bIrAgc) UpdateIrAgc(*In.NewHistogram, Cfg, In, bSnap);
			else        UpdateAe(*In.NewHistogram, Cfg, In, bSnap);
			if (bSnap) { bInitialized = true; bSnapPending = false; }
		}
	}
	else if (++TicksSinceHistogram == StaleAfterTicks)
	{
		++StaleEpisodes;
	}

	FSensorFrameParams P;
	P.Mode          = In.Mode;
	P.bBlackHot     = In.bBlackHot ? 1u : 0u;
	P.SignalWeights = Cfg.SignalWeights;
	P.Serial        = In.Serial;
	P.DisplayTint   = (In.Mode == ESensorGraphMode::NVG) ? FVector3f(0.3f, 1.0f, 0.3f) : FVector3f(1.0f, 1.0f, 1.0f);
	if (bIrAgc)
	{
		const float Lo = FMath::Exp2(IrLoLog2), Hi = FMath::Exp2(IrHiLog2);
		P.Gain   = 1.0f / FMath::Max(Hi - Lo, 1e-30f);
		P.Offset = -Lo * P.Gain;
		GainEv   = FMath::Log2(P.Gain);
	}
	else
	{
		P.Gain = FMath::Exp2(GainEv);
	}
	return P;
}
```

Note the snap test: `FSensorAeSnapTest` expects the pre-cut histogram (serial 4 < cut serial 5) to ease with `LagFrames = 30` — confirm the arithmetic in the test holds with `Smoothing` (one 1/30 s step at τ = 1 s moves ~3%, so `|gain − (Day+2)| > 1`).

- [ ] **Step 4: Build and run** `CamSim.Sensor.Controller`. Expected: all 9 pass.

- [ ] **Step 5: Commit** — `feat(sensor): FSensorController AE/AGC loop on sim time (3B.1)`.

---

### Task 4: Reference model and NV12 helpers

**Files:**
- Create: `Source/CamSimTest/Sensor/SensorReference.h`, `Source/CamSimTest/Sensor/SensorReference.cpp`
- Create: `Source/CamSimTest/Encoder/Nv12.h`, `Source/CamSimTest/Encoder/Nv12.cpp`
- Test: `Source/CamSimTest/Tests/SensorReferenceTest.cpp`, `Source/CamSimTest/Tests/Nv12Test.cpp`

**Interfaces:**
- Consumes: `FSensorFrameParams`, `FSensorHistogram` (Task 2).
- Produces:

```cpp
namespace CamSimSensorRef
{
	/** Order of operations — the GPU kernels (CamSimSensor.usf) follow this exactly:
	 *  1. c = sanitize(scene colour): NaN -> 0, clamp to [0, 65504]
	 *  2. c *= InputScale (runtime: View.OneOverPreExposure)
	 *  3. s = dot(c, SignalWeights); histogram[BinOf(s)]++
	 *  4. EO:     rgb = Oetf709(saturate(Knee(c * Gain, KneeStart)))           (per channel)
	 *     IR/NVG: v = saturate(s * Gain + Offset); if (bBlackHot) v = 1 - v;   rgb = v, luma source = v
	 *  5. NV12 (BT.709 limited range): per 4x2 block, Y per pixel, U/V from the 2x2 mean of R'G'B';
	 *     IR/NVG: Y from v, U = V = 128. */
	struct FResult
	{
		TArray<uint8>    Nv12;       // W*H*3/2
		FSensorHistogram Histogram;
	};
	float Sanitize(float V);
	float Knee(float X, float K);
	float Oetf709(float L);
	/** Scene: W*H linear RGBA (alpha ignored). W % 4 == 0, H % 2 == 0. */
	FResult Run(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P);
}

namespace CamSimNv12
{
	inline int32 NumBytes(int32 W, int32 H) { return W * H * 3 / 2; }
	/** Full-range BGRA from limited-range BT.709 NV12 (snapshots, tests). */
	void ToBgra(const uint8* Nv12, int32 W, int32 H, TArray<FColor>& Out);
	/** De-interleave NV12 into planar YUV420P (encoder input). */
	void SplitToYuv420p(const uint8* Nv12, int32 W, int32 H,
		uint8* Y, int32 YStride, uint8* U, int32 UStride, uint8* V, int32 VStride);
}
```

- [ ] **Step 1: Write the failing tests.** `Tests/Nv12Test.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Encoder/Nv12.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNv12SplitTest, "CamSim.Encoder.Nv12.SplitToYuv420p",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNv12SplitTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 8, H = 4;
	TArray<uint8> Nv12; Nv12.SetNumUninitialized(CamSimNv12::NumBytes(W, H));
	for (int32 I = 0; I < W * H; ++I) Nv12[I] = static_cast<uint8>(I);
	for (int32 I = 0; I < W * H / 2; I += 2) { Nv12[W * H + I] = 100 + I; Nv12[W * H + I + 1] = 200 + I; }
	uint8 Y[W * H], U[W * H / 4], V[W * H / 4];
	CamSimNv12::SplitToYuv420p(Nv12.GetData(), W, H, Y, W, U, W / 2, V, W / 2);
	TestEqual(TEXT("Y copied"), FMemory::Memcmp(Y, Nv12.GetData(), W * H), 0);
	for (int32 I = 0; I < W * H / 4; ++I)
	{
		TestEqual(TEXT("U"), (int32)U[I], 100 + 2 * I);
		TestEqual(TEXT("V"), (int32)V[I], 200 + 2 * I);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNv12ToBgraTest, "CamSim.Encoder.Nv12.ToBgraLimitedRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNv12ToBgraTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 4, H = 2;
	TArray<uint8> Nv12; Nv12.Init(128, CamSimNv12::NumBytes(W, H));
	Nv12[0] = 16; Nv12[1] = 235; Nv12[2] = 126;   // black, white, ~mid grey
	TArray<FColor> Out;
	CamSimNv12::ToBgra(Nv12.GetData(), W, H, Out);
	TestEqual(TEXT("black"), (int32)Out[0].R, 0);
	TestEqual(TEXT("white"), (int32)Out[1].G, 255);
	TestTrue(TEXT("grey"), FMath::Abs((int32)Out[2].B - 128) <= 1);
	TestEqual(TEXT("neutral chroma is grey"), (int32)Out[1].R, (int32)Out[1].B);
	return true;
}
```

`Tests/SensorReferenceTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Sensor/SensorReference.h"

namespace
{
	TArray<FLinearColor> Solid(int32 W, int32 H, FLinearColor C) { TArray<FLinearColor> A; A.Init(C, W * H); return A; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefEoGreyTest, "CamSim.Sensor.Reference.EoGreyLimitedRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefEoGreyTest::RunTest(const FString& Parameters)
{
	FSensorFrameParams P;  // EO, gain 1
	const auto R0 = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(0, 0, 0)), 8, 4, P);
	TestEqual(TEXT("black -> Y 16"), (int32)R0.Nv12[0], 16);
	TestEqual(TEXT("black chroma 128"), (int32)R0.Nv12[8 * 4], 128);
	const auto R1 = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(100, 100, 100)), 8, 4, P);
	TestEqual(TEXT("far over knee -> Y 235"), (int32)R1.Nv12[0], 235);
	const float Grey = 0.18f;
	const auto R2 = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(Grey, Grey, Grey)), 8, 4, P);
	const int32 Expected = FMath::RoundToInt32(16.0f + 219.0f * CamSimSensorRef::Oetf709(Grey));
	TestEqual(TEXT("0.18 through BT.709 OETF"), (int32)R2.Nv12[0], Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefHistogramTest, "CamSim.Sensor.Reference.HistogramCountsEveryPixel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefHistogramTest::RunTest(const FString& Parameters)
{
	// 1.03, not 1.0: the BT.709 weights may sum to 1 - 1 ulp, which would put 1.0 in bin 127.
	TArray<FLinearColor> Scene = Solid(8, 4, FLinearColor(1.03f, 1.03f, 1.03f));
	Scene[0] = FLinearColor(NAN, 0, 0);
	Scene[1] = FLinearColor(-5, -5, -5);
	Scene[2] = FLinearColor(INFINITY, INFINITY, INFINITY);
	FSensorFrameParams P;
	const auto R = CamSimSensorRef::Run(Scene, 8, 4, P);
	TestEqual(TEXT("total"), R.Histogram.Total(), (uint64)32);
	TestEqual(TEXT("NaN and negative in bin 0"), R.Histogram.Bins[0], 2u);
	TestEqual(TEXT("Inf clamps to 65504 -> bin of 65504"), R.Histogram.Bins[FSensorHistogram::BinOf(65504.0f)], 1u);
	TestEqual(TEXT("1.03 -> bin 128"), R.Histogram.Bins[128], 29u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefIrTest, "CamSim.Sensor.Reference.IrGainOffsetPolarity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefIrTest::RunTest(const FString& Parameters)
{
	FSensorFrameParams P;
	P.Mode = ESensorGraphMode::IR;
	P.Gain = 0.5f; P.Offset = 0.25f;   // s = 1 -> 0.75
	const auto White = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(1, 1, 1)), 8, 4, P);
	TestEqual(TEXT("white-hot"), (int32)White.Nv12[0], FMath::RoundToInt32(16 + 219 * 0.75f));
	TestEqual(TEXT("IR chroma neutral"), (int32)White.Nv12[8 * 4 + 1], 128);
	P.bBlackHot = 1;
	const auto Black = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(1, 1, 1)), 8, 4, P);
	TestEqual(TEXT("black-hot"), (int32)Black.Nv12[0], FMath::RoundToInt32(16 + 219 * 0.25f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefChromaTest, "CamSim.Sensor.Reference.EoChromaFrom2x2Mean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefChromaTest::RunTest(const FString& Parameters)
{
	FSensorFrameParams P;
	P.KneeStart = 1.0f;  // no knee: exact OETF
	const auto R = CamSimSensorRef::Run(Solid(4, 2, FLinearColor(0.5f, 0.0f, 0.0f)), 4, 2, P);
	const float Rp = CamSimSensorRef::Oetf709(0.5f);
	const float Y = 0.2126f * Rp;
	TestEqual(TEXT("Y"), (int32)R.Nv12[0], FMath::RoundToInt32(16 + 219 * Y));
	TestEqual(TEXT("Cb"), (int32)R.Nv12[8], FMath::RoundToInt32(128 + 224 * (0.0f - Y) / 1.8556f));
	TestEqual(TEXT("Cr"), (int32)R.Nv12[9], FMath::RoundToInt32(128 + 224 * (Rp - Y) / 1.5748f));
	return true;
}
```

- [ ] **Step 2: Build** — expected: fails (headers missing).

- [ ] **Step 3: Implement `Encoder/Nv12.{h,cpp}`** — header declares the `CamSimNv12` namespace from **Interfaces**. `Nv12.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Encoder/Nv12.h"

namespace CamSimNv12
{
	void SplitToYuv420p(const uint8* Nv12, int32 W, int32 H,
		uint8* Y, int32 YStride, uint8* U, int32 UStride, uint8* V, int32 VStride)
	{
		for (int32 Row = 0; Row < H; ++Row)
		{
			FMemory::Memcpy(Y + Row * YStride, Nv12 + Row * W, W);
		}
		const uint8* UV = Nv12 + W * H;
		for (int32 Row = 0; Row < H / 2; ++Row)
		{
			const uint8* Src = UV + Row * W;
			uint8* DU = U + Row * UStride;
			uint8* DV = V + Row * VStride;
			for (int32 X = 0; X < W / 2; ++X) { DU[X] = Src[2 * X]; DV[X] = Src[2 * X + 1]; }
		}
	}

	void ToBgra(const uint8* Nv12, int32 W, int32 H, TArray<FColor>& Out)
	{
		Out.SetNumUninitialized(W * H);
		const uint8* UV = Nv12 + W * H;
		for (int32 Row = 0; Row < H; ++Row)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const float Yn = (Nv12[Row * W + X] - 16.0f) / 219.0f;
				const int32 C = (Row / 2) * W + (X / 2) * 2;
				const float Cb = (UV[C] - 128.0f) / 224.0f;
				const float Cr = (UV[C + 1] - 128.0f) / 224.0f;
				// BT.709 inverse (Kr = 0.2126, Kb = 0.0722)
				const float R = Yn + 1.5748f * Cr;
				const float G = Yn - 0.1873f * Cb - 0.4681f * Cr;
				const float B = Yn + 1.8556f * Cb;
				auto To8 = [](float V) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(V * 255.0f), 0, 255)); };
				Out[Row * W + X] = FColor(To8(R), To8(G), To8(B), 255);
			}
		}
	}
}
```

- [ ] **Step 4: Implement `Sensor/SensorReference.{h,cpp}`** — header: copyright, `CoreMinimal.h`, `SensorFrameParams.h`, the `CamSimSensorRef` namespace from **Interfaces** (including the order-of-operations comment verbatim; the `.usf` references it). `.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorReference.h"

namespace CamSimSensorRef
{
	float Sanitize(float V)
	{
		if (FMath::IsNaN(V)) return 0.0f;
		return FMath::Clamp(V, 0.0f, 65504.0f);   // +Inf -> 65504
	}

	float Knee(float X, float K)
	{
		if (X <= K || K >= 1.0f) return X;
		return K + (1.0f - K) * (1.0f - FMath::Exp(-(X - K) / (1.0f - K)));
	}

	float Oetf709(float L)
	{
		L = FMath::Clamp(L, 0.0f, 1.0f);
		return L < 0.018f ? 4.5f * L : 1.099f * FMath::Pow(L, 0.45f) - 0.099f;
	}

	static uint8 ToLimited(float V, float Scale) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(16.0f + Scale * V), 0, 255)); }
	static uint8 ToChroma(float C) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(128.0f + 224.0f * C), 0, 255)); }

	FResult Run(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P)
	{
		check(Scene.Num() == W * H && W % 4 == 0 && H % 2 == 0);
		FResult R;
		R.Nv12.SetNumZeroed(W * H * 3 / 2);
		TArray<FVector4f> Out;   // xyz = R'G'B' (EO) or v,v,v; w = luma source (IR/NVG)
		Out.SetNumUninitialized(W * H);
		const bool bEo = P.Mode == ESensorGraphMode::EO;
		for (int32 I = 0; I < W * H; ++I)
		{
			const FVector3f C(Sanitize(Scene[I].R) * P.InputScale, Sanitize(Scene[I].G) * P.InputScale, Sanitize(Scene[I].B) * P.InputScale);
			const float S = C.X * P.SignalWeights.X + C.Y * P.SignalWeights.Y + C.Z * P.SignalWeights.Z;
			++R.Histogram.Bins[FSensorHistogram::BinOf(S)];
			if (bEo)
			{
				Out[I] = FVector4f(Oetf709(Knee(C.X * P.Gain, P.KneeStart)), Oetf709(Knee(C.Y * P.Gain, P.KneeStart)),
					Oetf709(Knee(C.Z * P.Gain, P.KneeStart)), 1.0f);
			}
			else
			{
				float V = FMath::Clamp(S * P.Gain + P.Offset, 0.0f, 1.0f);
				if (P.bBlackHot) V = 1.0f - V;
				Out[I] = FVector4f(V, V, V, V);
			}
		}
		uint8* Y = R.Nv12.GetData();
		uint8* UV = Y + W * H;
		for (int32 Row = 0; Row < H; Row += 2)
		{
			for (int32 X = 0; X < W; X += 2)
			{
				FVector3f Sum(0, 0, 0);
				for (int32 Dy = 0; Dy < 2; ++Dy)
				{
					for (int32 Dx = 0; Dx < 2; ++Dx)
					{
						const FVector4f& O = Out[(Row + Dy) * W + X + Dx];
						const float Luma = bEo ? (0.2126f * O.X + 0.7152f * O.Y + 0.0722f * O.Z) : O.W;
						Y[(Row + Dy) * W + X + Dx] = ToLimited(Luma, 219.0f);
						Sum += FVector3f(O.X, O.Y, O.Z);
					}
				}
				const FVector3f M = Sum * 0.25f;
				const float Ym = 0.2126f * M.X + 0.7152f * M.Y + 0.0722f * M.Z;
				UV[(Row / 2) * W + X]     = bEo ? ToChroma((M.Z - Ym) / 1.8556f) : 128;
				UV[(Row / 2) * W + X + 1] = bEo ? ToChroma((M.X - Ym) / 1.5748f) : 128;
			}
		}
		return R;
	}
}
```

- [ ] **Step 5: Build and run** `CamSim.Sensor.Reference` and `CamSim.Encoder.Nv12`. Expected: all pass.

- [ ] **Step 6: Commit** — `feat(sensor): scalar reference model and NV12 helpers (3B.1)`.

---

### Task 5: Compute shaders, `AddSensorPasses`, GPU-vs-reference tests

**Files:**
- Modify: `unreal_project/CamSimTest/Shaders/Private/CamSimSensor.usf`
- Create: `Source/CamSimShaders/Public/SensorGraph.h`, `Source/CamSimShaders/Private/SensorGraph.cpp`
- Create: `Source/CamSimTest/Tests/SensorGpuTest.cpp`, `scripts/run_gpu_tests.sh`
- Modify: `CLAUDE.md` (Testing section: the GPU test command)

**Interfaces:**
- Consumes: `FSensorFrameParams`, `FSensorHistogram` (Task 2); `CamSimSensorRef::Run` (Task 4, tests only).
- Produces (`SensorGraph.h`, exported with `CAMSIMSHADERS_API`):

```cpp
struct FSensorGraphInputs
{
	FRDGTextureRef SceneColor = nullptr;       // HDR, pre-exposed unless View is null
	FIntRect       SceneViewRect;              // region of SceneColor to sample
	FRDGTextureRef Bloom = nullptr;            // optional (UE CombinedBloom), same units as SceneColor
	FIntRect       BloomViewRect;
	FRHIUniformBuffer* ViewUniformBuffer = nullptr;  // runtime: divides out View.OneOverPreExposure; null in tests (InputScale used)
	FIntPoint      OutputSize = FIntPoint::ZeroValue;  // capture size; X % 4 == 0, Y % 2 == 0
};

struct FSensorGraphOutputs
{
	FRDGTextureRef SensorRgb = nullptr;   // OutputSize, PF_FloatRGBA: display RGB (tinted), .a = luma source
	FRDGBufferRef  Nv12      = nullptr;   // OutputSize.X*Y*3/2 bytes as uint32 structured buffer
	FRDGBufferRef  Histogram = nullptr;   // 256 x uint32 structured buffer
	uint32 Nv12Bytes = 0;
};

CAMSIMSHADERS_API FSensorGraphOutputs AddSensorPasses(FRDGBuilder& GraphBuilder, const FSensorGraphInputs& In,
	const FSensorFrameParams& Params);
```

- [ ] **Step 1: Write the failing GPU tests** — `Tests/SensorGpuTest.cpp`. These skip under NullRHI and run for real via `scripts/run_gpu_tests.sh`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SensorGraph.h"
#include "Sensor/SensorReference.h"

namespace
{
	struct FGpuResult { TArray<uint8> Nv12; FSensorHistogram Histogram; bool bOk = false; };

	/** Upload Scene (W*H float RGBA), run AddSensorPasses, read back NV12 + histogram synchronously. */
	FGpuResult RunOnGpu(const TArray<FLinearColor>& Scene, int32 SrcW, int32 SrcH, FIntPoint OutSize, const FSensorFrameParams& P)
	{
		FGpuResult Result;
		ENQUEUE_RENDER_COMMAND(CamSimSensorGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestScene"), SrcW, SrcH, PF_A32B32G32R32F)
				.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
			FTextureRHIRef Tex = RHICreateTexture(Desc);
			RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, SrcW, SrcH), SrcW * sizeof(FLinearColor),
				reinterpret_cast<const uint8*>(Scene.GetData()));

			FRHIGPUBufferReadback Nv12Rb(TEXT("CamSimTestNv12"));
			FRHIGPUBufferReadback HistRb(TEXT("CamSimTestHist"));
			uint32 Nv12Bytes = 0;
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FSensorGraphInputs In;
				In.SceneColor = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Tex, TEXT("CamSimTestScene")));
				In.SceneViewRect = FIntRect(0, 0, SrcW, SrcH);
				In.OutputSize = OutSize;
				const FSensorGraphOutputs Out = AddSensorPasses(GraphBuilder, In, P);
				Nv12Bytes = Out.Nv12Bytes;
				AddEnqueueCopyPass(GraphBuilder, &Nv12Rb, Out.Nv12, Nv12Bytes);
				AddEnqueueCopyPass(GraphBuilder, &HistRb, Out.Histogram, FSensorHistogram::NumBins * sizeof(uint32));
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitCommandsAndFlushGPU();
			RHICmdList.BlockUntilGPUIdle();
			if (!Nv12Rb.IsReady() || !HistRb.IsReady()) return;
			Result.Nv12.SetNumUninitialized(Nv12Bytes);
			FMemory::Memcpy(Result.Nv12.GetData(), Nv12Rb.Lock(Nv12Bytes), Nv12Bytes);
			Nv12Rb.Unlock();
			FMemory::Memcpy(Result.Histogram.Bins.GetData(), HistRb.Lock(FSensorHistogram::NumBins * sizeof(uint32)),
				FSensorHistogram::NumBins * sizeof(uint32));
			HistRb.Unlock();
			Result.bOk = true;
		});
		FlushRenderingCommands();
		return Result;
	}

	void Compare(FAutomationTestBase& T, const FGpuResult& G, const CamSimSensorRef::FResult& R, int32 W, int32 H, bool bCheckHistogram = true)
	{
		if (!T.TestTrue(TEXT("GPU readback"), G.bOk && G.Nv12.Num() == R.Nv12.Num())) return;
		int32 MaxY = 0, MaxC = 0;
		for (int32 I = 0; I < W * H; ++I) MaxY = FMath::Max(MaxY, FMath::Abs((int32)G.Nv12[I] - (int32)R.Nv12[I]));
		for (int32 I = W * H; I < R.Nv12.Num(); ++I) MaxC = FMath::Max(MaxC, FMath::Abs((int32)G.Nv12[I] - (int32)R.Nv12[I]));
		T.TestTrue(FString::Printf(TEXT("Y within 1 DN (max %d)"), MaxY), MaxY <= 1);
		T.TestTrue(FString::Printf(TEXT("UV within 2 DN (max %d)"), MaxC), MaxC <= 2);
		if (bCheckHistogram)
		{
			for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
			{
				if (G.Histogram.Bins[B] != R.Histogram.Bins[B])
				{
					T.AddError(FString::Printf(TEXT("histogram bin %d: GPU %u, reference %u"), B, G.Histogram.Bins[B], R.Histogram.Bins[B]));
					break;
				}
			}
		}
	}

	/** Grey pixels at histogram bin centres covering all 256 bins (a 32-stop log gradient). */
	TArray<FLinearColor> LogGradient(int32 W, int32 H)
	{
		TArray<FLinearColor> A; A.SetNumUninitialized(W * H);
		for (int32 I = 0; I < W * H; ++I)
		{
			const float V = FMath::Exp2(FSensorHistogram::BinCentreLog2(I % FSensorHistogram::NumBins));
			A[I] = FLinearColor(V, V, V, 1);
		}
		return A;
	}

	bool SkipWithoutGpu(FAutomationTestBase& T)
	{
		if (GUsingNullRHI) { T.AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuEoTest, "CamSim.GPU.Sensor.EoLogGradient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuEoTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	const TArray<FLinearColor> Scene = LogGradient(W, H);
	FSensorFrameParams P; P.Gain = FMath::Exp2(-4.0f);
	Compare(*this, RunOnGpu(Scene, W, H, FIntPoint(W, H), P), CamSimSensorRef::Run(Scene, W, H, P), W, H);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuIrTest, "CamSim.GPU.Sensor.IrStepEdgeBlackHot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuIrTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<FLinearColor> Scene; Scene.SetNumUninitialized(W * H);
	for (int32 I = 0; I < W * H; ++I) { const float V = (I % W) < W / 2 ? 0.5f : 8.0f; Scene[I] = FLinearColor(V, V, V, 1); }
	FSensorFrameParams P; P.Mode = ESensorGraphMode::IR; P.Gain = 0.1f; P.Offset = 0.05f; P.bBlackHot = 1;
	Compare(*this, RunOnGpu(Scene, W, H, FIntPoint(W, H), P), CamSimSensorRef::Run(Scene, W, H, P), W, H);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuNvgTest, "CamSim.GPU.Sensor.NvgNight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuNvgTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<FLinearColor> Scene = LogGradient(W, H);
	for (FLinearColor& C : Scene) { C.R *= 1e-6f; C.G *= 0.5e-6f; C.B *= 0.2e-6f; }  // starlight, red-heavy
	FSensorFrameParams P; P.Mode = ESensorGraphMode::NVG; P.SignalWeights = FVector3f(0.6f, 0.3f, 0.1f); P.Gain = FMath::Exp2(14.0f);
	// Histogram: coloured input no longer sits on bin centres, so compare NV12 only here.
	Compare(*this, RunOnGpu(Scene, W, H, FIntPoint(W, H), P), CamSimSensorRef::Run(Scene, W, H, P), W, H, /*bCheckHistogram=*/false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuSanitizeTest, "CamSim.GPU.Sensor.SanitizesNaNInfNegative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuSanitizeTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<FLinearColor> Scene = LogGradient(W, H);
	for (int32 I = 0; I < W * H; I += 7) Scene[I] = FLinearColor(NAN, NAN, NAN, 1);
	for (int32 I = 3; I < W * H; I += 11) Scene[I] = FLinearColor(INFINITY, INFINITY, INFINITY, 1);
	for (int32 I = 5; I < W * H; I += 13) Scene[I] = FLinearColor(-1, -1, -1, 1);
	FSensorFrameParams P; P.Gain = FMath::Exp2(-4.0f);
	Compare(*this, RunOnGpu(Scene, W, H, FIntPoint(W, H), P), CamSimSensorRef::Run(Scene, W, H, P), W, H);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuScaledTest, "CamSim.GPU.Sensor.GpuMatchesReferenceScaled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuScaledTest::RunTest(const FString& Parameters)
{
	// View 2x the capture (Retina): the graph resamples bilinearly. A 2x2-constant
	// scene makes bilinear at the 2x2 centre exact, so the reference is the 1x image.
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 32, H = 16;
	const TArray<FLinearColor> Small = LogGradient(W, H);
	TArray<FLinearColor> Big; Big.SetNumUninitialized(4 * W * H);
	for (int32 Y = 0; Y < 2 * H; ++Y) for (int32 X = 0; X < 2 * W; ++X) Big[Y * 2 * W + X] = Small[(Y / 2) * W + X / 2];
	FSensorFrameParams P; P.Gain = FMath::Exp2(-4.0f);
	Compare(*this, RunOnGpu(Big, 2 * W, 2 * H, FIntPoint(W, H), P), CamSimSensorRef::Run(Small, W, H, P), W, H);
	return true;
}
```

`AddEnqueueCopyPass` for buffers exists in `RenderGraphUtils.h` (`AddEnqueueCopyPass(FRDGBuilder&, FRHIGPUBufferReadback*, FRDGBufferRef, uint32 NumBytes)`); if the 5.8 signature differs, use `AddReadbackBufferPass` with a lambda calling `Readback->EnqueueCopy(RHICmdList, Buffer->GetRHI(), NumBytes)`. Likewise check `RHICreateTexture` vs `RHICmdList.CreateTexture` in `RHICommandList.h` and use what 5.8 offers.

- [ ] **Step 2: Create `scripts/run_gpu_tests.sh`:**

```bash
#!/usr/bin/env bash
# GPU automation tests (CamSim.GPU.*) on the real RHI (Metal on macOS).
# NullRHI runs skip them. First run compiles the global shaders (minutes).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
UE_BIN="${UE_BIN:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor}"
FILTER="${1:-CamSim.GPU}"
OUT="$REPO/.cache/automation-report-gpu"
rm -rf "$OUT"
"$UE_BIN" "$REPO/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests ${FILTER}+Quit" -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="$OUT" -unattended -nosound -nosplash -DisablePython -RenderOffscreen \
  -log -stdout -FullStdOutLogOutput > "$REPO/.cache/automation-gpu.log" 2>&1 || true
python3 - "$OUT/index.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8-sig"))
for t in d["tests"]:
    if t["state"] == "Fail":
        msgs = [e["event"]["message"] for e in t["entries"] if e["event"]["type"] == "Error"]
        print("FAIL", t["fullTestPath"], msgs)
print("succeeded", d["succeeded"], "failed", d["failed"])
sys.exit(1 if d["failed"] or d["succeeded"] == 0 else 0)
EOF
```

`chmod +x scripts/run_gpu_tests.sh`.

- [ ] **Step 3: Build and run the tests** — build fails (`SensorGraph.h` missing). That's the failing state.

- [ ] **Step 4: Write the kernels** — replace `Shaders/Private/CamSimSensor.usf`:

```hlsl
// Copyright CamSim Contributors. All Rights Reserved.
// GPU sensor model (ROADMAP 3B). Order of operations: see Sensor/SensorReference.h.
// Plain compute: integer atomics only, no wave intrinsics (Metal + Vulkan).

#include "/Engine/Public/Platform.ush"
#if USE_VIEW_PREEXPOSURE
#include "/Engine/Private/Common.ush"   // View.OneOverPreExposure
#endif

#define HIST_BINS 256
#define HIST_MIN_LOG2 (-16.0)
#define HIST_BINS_PER_STOP 8.0

Texture2D<float4> SceneColor;
SamplerState SceneColorSampler;
float2 SceneUvMin;      // UV of the view rect's top-left
float2 SceneUvSize;     // UV extent of the view rect
int2   SceneViewMin;    // texel of the view rect's top-left (exact path)
uint   bExactInput;     // view rect size == OutputSize: Load instead of sampling
Texture2D<float4> Bloom;
SamplerState BloomSampler;
float2 BloomUvMin;
float2 BloomUvSize;
float  BloomWeight;     // 0 without bloom
float  InputScale;      // tests; runtime multiplies View.OneOverPreExposure in as well
int2   OutputSize;
uint   Mode;            // 0 EO, 1 IR, 2 NVG
uint   bBlackHot;
float  Gain;
float  Offset;
float3 SignalWeights;
float  KneeStart;
float3 DisplayTint;

RWTexture2D<float4> OutSensorRgb;
RWStructuredBuffer<uint> OutHistogram;

groupshared uint LocalHistogram[HIST_BINS];

float Sanitize(float V) { return (V != V) ? 0.0 : clamp(V, 0.0, 65504.0); }

float Knee(float X, float K)
{
	if (X <= K || K >= 1.0) return X;
	return K + (1.0 - K) * (1.0 - exp(-(X - K) / (1.0 - K)));
}

float Oetf709(float L)
{
	L = saturate(L);
	return L < 0.018 ? 4.5 * L : 1.099 * pow(L, 0.45) - 0.099;
}

uint BinOf(float S)
{
	if (!(S > 0.0)) return 0;
	return (uint)clamp((int)floor((log2(S) - HIST_MIN_LOG2) * HIST_BINS_PER_STOP), 0, HIST_BINS - 1);
}

[numthreads(8, 8, 1)]
void ApplyCS(uint3 DispatchThreadId : SV_DispatchThreadID, uint GroupIndex : SV_GroupIndex)
{
	for (uint I = GroupIndex; I < HIST_BINS; I += 64) LocalHistogram[I] = 0;
	GroupMemoryBarrierWithGroupSync();

	const int2 P = int2(DispatchThreadId.xy);
	if (all(P < OutputSize))
	{
		const float2 Frac = (float2(P) + 0.5) / float2(OutputSize);
		float3 C = (bExactInput != 0)
			? SceneColor.Load(int3(SceneViewMin + P, 0)).rgb
			: SceneColor.SampleLevel(SceneColorSampler, SceneUvMin + Frac * SceneUvSize, 0).rgb;
		if (BloomWeight > 0.0)
		{
			C += BloomWeight * Bloom.SampleLevel(BloomSampler, BloomUvMin + Frac * BloomUvSize, 0).rgb;
		}
		float Scale = InputScale;
#if USE_VIEW_PREEXPOSURE
		Scale *= View.OneOverPreExposure;
#endif
		C = float3(Sanitize(C.r), Sanitize(C.g), Sanitize(C.b)) * Scale;
		const float S = dot(C, SignalWeights);
		InterlockedAdd(LocalHistogram[BinOf(S)], 1u);

		float4 Out;
		if (Mode == 0)
		{
			const float3 G = C * Gain;
			Out = float4(Oetf709(Knee(G.r, KneeStart)), Oetf709(Knee(G.g, KneeStart)), Oetf709(Knee(G.b, KneeStart)), 1.0);
		}
		else
		{
			float V = saturate(S * Gain + Offset);
			if (bBlackHot != 0) V = 1.0 - V;
			Out = float4(V * DisplayTint, V);
		}
		OutSensorRgb[P] = Out;
	}

	GroupMemoryBarrierWithGroupSync();
	for (uint J = GroupIndex; J < HIST_BINS; J += 64)
	{
		if (LocalHistogram[J] != 0) InterlockedAdd(OutHistogram[J], LocalHistogram[J]);
	}
}

Texture2D<float4> SensorRgb;
RWStructuredBuffer<uint> OutNv12;

uint ToLimited(float V) { return (uint)clamp((int)round(16.0 + 219.0 * V), 0, 255); }
uint ToChroma(float C)  { return (uint)clamp((int)round(128.0 + 224.0 * C), 0, 255); }

float LumaOf(float4 O) { return (Mode == 0) ? dot(O.rgb, float3(0.2126, 0.7152, 0.0722)) : O.a; }

/** One thread per 4x2 block: two uints of Y, one uint of UV (U0 V0 U1 V1). */
[numthreads(8, 8, 1)]
void PackNv12CS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
	const int2 Block = int2(DispatchThreadId.xy);
	const int2 P0 = Block * int2(4, 2);
	if (any(P0 >= OutputSize)) return;

	uint Y0 = 0, Y1 = 0, UV = 0;
	[unroll] for (int Pair = 0; Pair < 2; ++Pair)
	{
		float3 Sum = 0;
		[unroll] for (int Dx = 0; Dx < 2; ++Dx)
		{
			const int X = P0.x + Pair * 2 + Dx;
			const float4 A = SensorRgb.Load(int3(X, P0.y, 0));
			const float4 B = SensorRgb.Load(int3(X, P0.y + 1, 0));
			const uint Shift = (uint)(Pair * 2 + Dx) * 8;
			Y0 |= ToLimited(LumaOf(A)) << Shift;
			Y1 |= ToLimited(LumaOf(B)) << Shift;
			Sum += A.rgb + B.rgb;
		}
		const float3 M = Sum * 0.25;
		const float Ym = dot(M, float3(0.2126, 0.7152, 0.0722));
		const uint U = (Mode == 0) ? ToChroma((M.b - Ym) / 1.8556) : 128;
		const uint V = (Mode == 0) ? ToChroma((M.r - Ym) / 1.5748) : 128;
		UV |= (U << (Pair * 16)) | (V << (Pair * 16 + 8));
	}
	const uint W = (uint)OutputSize.x;
	OutNv12[(P0.y * W + P0.x) / 4]           = Y0;
	OutNv12[((P0.y + 1) * W + P0.x) / 4]     = Y1;
	OutNv12[(W * (uint)OutputSize.y + (P0.y / 2) * W + P0.x) / 4] = UV;
}
```

Note: for NVG, `SensorRgb.rgb` is tinted, but chroma is forced to 128 and luma comes from `.a`, so the stream stays untinted luma. The reference does the same (tint does not exist in the reference; only luma/`.a` is compared).

- [ ] **Step 5: Write `SensorGraph.{h,cpp}`** — header: copyright, `#pragma once`, `CoreMinimal.h`, `RenderGraphDefinitions.h`, `SensorFrameParams.h`, forward declare `FRHIUniformBuffer`, then the **Interfaces** declarations. `.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "SensorGraph.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "SceneView.h"
#include "SystemTextures.h"

class FCamSimSensorApplyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCamSimSensorApplyCS);
	SHADER_USE_PARAMETER_STRUCT(FCamSimSensorApplyCS, FGlobalShader);

	class FUseViewPreExposure : SHADER_PERMUTATION_BOOL("USE_VIEW_PREEXPOSURE");
	using FPermutationDomain = TShaderPermutationDomain<FUseViewPreExposure>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
		SHADER_PARAMETER(FVector2f, SceneUvMin)
		SHADER_PARAMETER(FVector2f, SceneUvSize)
		SHADER_PARAMETER(FIntPoint, SceneViewMin)
		SHADER_PARAMETER(uint32, bExactInput)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Bloom)
		SHADER_PARAMETER_SAMPLER(SamplerState, BloomSampler)
		SHADER_PARAMETER(FVector2f, BloomUvMin)
		SHADER_PARAMETER(FVector2f, BloomUvSize)
		SHADER_PARAMETER(float, BloomWeight)
		SHADER_PARAMETER(float, InputScale)
		SHADER_PARAMETER(FIntPoint, OutputSize)
		SHADER_PARAMETER(uint32, Mode)
		SHADER_PARAMETER(uint32, bBlackHot)
		SHADER_PARAMETER(float, Gain)
		SHADER_PARAMETER(float, Offset)
		SHADER_PARAMETER(FVector3f, SignalWeights)
		SHADER_PARAMETER(float, KneeStart)
		SHADER_PARAMETER(FVector3f, DisplayTint)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSensorRgb)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutHistogram)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCamSimSensorApplyCS, "/CamSim/Private/CamSimSensor.usf", "ApplyCS", SF_Compute);

class FCamSimSensorPackNv12CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCamSimSensorPackNv12CS);
	SHADER_USE_PARAMETER_STRUCT(FCamSimSensorPackNv12CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SensorRgb)
		SHADER_PARAMETER(FIntPoint, OutputSize)
		SHADER_PARAMETER(uint32, Mode)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutNv12)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCamSimSensorPackNv12CS, "/CamSim/Private/CamSimSensor.usf", "PackNv12CS", SF_Compute);

FSensorGraphOutputs AddSensorPasses(FRDGBuilder& GraphBuilder, const FSensorGraphInputs& In, const FSensorFrameParams& P)
{
	check(In.SceneColor && In.OutputSize.X % 4 == 0 && In.OutputSize.Y % 2 == 0);
	RDG_EVENT_SCOPE(GraphBuilder, "CamSimSensor");
	const FIntPoint Out = In.OutputSize;

	FSensorGraphOutputs Result;
	Result.SensorRgb = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(Out, PF_FloatRGBA, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("CamSimSensorRgb"));
	Result.Histogram = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FSensorHistogram::NumBins),
		TEXT("CamSimSensorHistogram"));
	Result.Nv12Bytes = static_cast<uint32>(Out.X * Out.Y * 3 / 2);
	Result.Nv12 = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Result.Nv12Bytes / 4),
		TEXT("CamSimSensorNv12"));
	FRDGBufferUAVRef HistUav = GraphBuilder.CreateUAV(Result.Histogram);
	AddClearUAVPass(GraphBuilder, HistUav, 0u);

	const FIntPoint SrcExtent = In.SceneColor->Desc.Extent;
	const FIntPoint SrcSize = In.SceneViewRect.Size();
	{
		const bool bUseView = In.ViewUniformBuffer != nullptr;
		auto* Pass = GraphBuilder.AllocParameters<FCamSimSensorApplyCS::FParameters>();
		if (bUseView)
		{
			Pass->View = TUniformBufferRef<FViewUniformShaderParameters>(In.ViewUniformBuffer);
		}
		Pass->SceneColor        = In.SceneColor;
		Pass->SceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		Pass->SceneUvMin        = FVector2f(In.SceneViewRect.Min) / FVector2f(SrcExtent);
		Pass->SceneUvSize       = FVector2f(SrcSize) / FVector2f(SrcExtent);
		Pass->SceneViewMin      = In.SceneViewRect.Min;
		Pass->bExactInput       = (SrcSize == Out) ? 1u : 0u;
		const bool bBloom = In.Bloom != nullptr;
		Pass->Bloom        = bBloom ? In.Bloom : GSystemTextures.GetBlackDummy(GraphBuilder);
		Pass->BloomSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		if (bBloom)
		{
			const FVector2f BloomExtent(In.Bloom->Desc.Extent);
			Pass->BloomUvMin  = FVector2f(In.BloomViewRect.Min) / BloomExtent;
			Pass->BloomUvSize = FVector2f(In.BloomViewRect.Size()) / BloomExtent;
		}
		Pass->BloomWeight   = bBloom ? 1.0f : 0.0f;
		Pass->InputScale    = P.InputScale;
		Pass->OutputSize    = Out;
		Pass->Mode          = static_cast<uint32>(P.Mode);
		Pass->bBlackHot     = P.bBlackHot;
		Pass->Gain          = P.Gain;
		Pass->Offset        = P.Offset;
		Pass->SignalWeights = P.SignalWeights;
		Pass->KneeStart     = P.KneeStart;
		Pass->DisplayTint   = P.DisplayTint;
		Pass->OutSensorRgb  = GraphBuilder.CreateUAV(Result.SensorRgb);
		Pass->OutHistogram  = HistUav;

		FCamSimSensorApplyCS::FPermutationDomain Perm;
		Perm.Set<FCamSimSensorApplyCS::FUseViewPreExposure>(bUseView);
		TShaderMapRef<FCamSimSensorApplyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Perm);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Apply %dx%d", Out.X, Out.Y), Shader, Pass,
			FComputeShaderUtils::GetGroupCount(Out, FIntPoint(8, 8)));
	}
	{
		auto* Pass = GraphBuilder.AllocParameters<FCamSimSensorPackNv12CS::FParameters>();
		Pass->SensorRgb  = Result.SensorRgb;
		Pass->OutputSize = Out;
		Pass->Mode       = static_cast<uint32>(P.Mode);
		Pass->OutNv12    = GraphBuilder.CreateUAV(Result.Nv12);
		TShaderMapRef<FCamSimSensorPackNv12CS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("PackNv12"), Shader, Pass,
			FComputeShaderUtils::GetGroupCount(FIntPoint(Out.X / 4, Out.Y / 2), FIntPoint(8, 8)));
	}
	return Result;
}
```

`GSystemTextures` lives in the Renderer's private `SystemTextures.h` in some versions; if it isn't reachable from a public include, create a 1×1 black `PF_FloatRGBA` texture with `GraphBuilder.CreateTexture` + `AddClearRenderTargetPass(GraphBuilder, Tex, FLinearColor::Black)` instead. The NV12 and histogram buffers need the copy-source usage for readback; structured buffers created by RDG get it automatically when a readback pass reads them — if validation complains, add `EBufferUsageFlags::SourceCopy` to the desc's `Usage`.

- [ ] **Step 6: Build; run NullRHI `CamSim.GPU` (expect 5 tests, all pass with "skipped" info) then `scripts/run_gpu_tests.sh`** (expect 5 passed, 0 failed; first run compiles shaders). If a comparison fails, print the first differing pixel's input, GPU and reference values before changing tolerances — tolerances are fixed by the spec.

- [ ] **Step 7: Document** in `CLAUDE.md` Testing: "**GPU tests** (`CamSim.GPU.*`, real RHI; skipped under NullRHI): `scripts/run_gpu_tests.sh [filter]` (Metal on macOS; first run compiles shaders)".

- [ ] **Step 8: Commit** — `feat(sensor): RDG sensor kernels with GPU-vs-reference tests (3B.1)`.

---

### Task 6: Path selector, stats mailbox, path reporting

**Files:**
- Create: `Source/CamSimTest/Sensor/SensorPath.{h,cpp}`, `Source/CamSimTest/Sensor/SensorStatsMailbox.h`
- Modify: `Source/CamSimTest/Tests/SensorPathTest.cpp` (add tests), `Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` (metrics + health JSON), `Source/CamSimTest/Camera/CamSimCamera.h` (getter)

**Interfaces:**
- Consumes: `FCamSimConfig` (Task 1), `FSensorHistogram` (Task 2).
- Produces:

```cpp
enum class ESensorPipelinePath : uint8 { Legacy = 0, Gpu = 1 };
struct FSensorPathDecision
{
	ESensorPipelinePath Path = ESensorPipelinePath::Legacy;
	TArray<FString> Unported;   // enabled effects the GPU path doesn't support yet
	FString Reason;             // one log line
};
struct FSensorPathSelector
{
	static FSensorPathDecision Decide(const FCamSimConfig& Cfg);
	static const TCHAR* ToString(ESensorPipelinePath P) { return P == ESensorPipelinePath::Gpu ? TEXT("gpu") : TEXT("legacy"); }
};

/** Newest histogram, render thread → game thread. Older unread ones are overwritten. */
class FSensorStatsMailbox
{
public:
	void Publish(const FSensorHistogram& H) { FScopeLock L(&Lock); Latest = H; bFresh = true; }
	bool TakeLatest(FSensorHistogram& Out) { FScopeLock L(&Lock); if (!bFresh) return false; Out = Latest; bFresh = false; return true; }
private:
	FCriticalSection Lock;
	FSensorHistogram Latest;
	bool bFresh = false;
};
```

- This task adds only the selector, the mailbox and their tests. Wiring them in (`ACamSimCamera::GetSensorPath()`, `/metrics` and health-JSON reporting) is Task 8.

- [ ] **Step 1: Write the failing tests** — append to `Tests/SensorPathTest.cpp` (add `#include "Sensor/SensorPath.h"`, `#include "Sensor/SensorStatsMailbox.h"`, `#include "Config/CamSimConfig.h"`):

```cpp
namespace
{
	FCamSimConfig CleanGpuConfig()
	{
		FCamSimConfig Cfg;
		for (ESensorMode M : { ESensorMode::EO, ESensorMode::IR, ESensorMode::NVG })
		{
			Cfg.SensorModeConfigs.Add(M, FSensorModeConfig());
			FSensorModeConfig& C = Cfg.SensorModeConfigs[M];
			C.Vignetting = 0.0f;   // default is 0.15: unported in 3B.1
		}
		Cfg.OverlayConfig.bEnabled = false;
		return Cfg;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPathAutoTest, "CamSim.Sensor.Path.AutoPicksGpuOnlyWhenAllPorted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPathAutoTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg = CleanGpuConfig();
	TestTrue(TEXT("clean config -> gpu"), FSensorPathSelector::Decide(Cfg).Path == ESensorPipelinePath::Gpu);

	Cfg.SensorModeConfigs[ESensorMode::IR].NETD = 0.01f;
	Cfg.LaserDesignator.bEnabled = true;
	const FSensorPathDecision D = FSensorPathSelector::Decide(Cfg);
	TestTrue(TEXT("unported -> legacy"), D.Path == ESensorPipelinePath::Legacy);
	TestTrue(TEXT("names noise"), D.Unported.ContainsByPredicate([](const FString& S) { return S.Contains(TEXT("noise_netd")); }));
	TestTrue(TEXT("names laser"), D.Unported.Contains(TEXT("laser_designator")));
	TestTrue(TEXT("reason lists them"), D.Reason.Contains(TEXT("laser_designator")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPathForcedTest, "CamSim.Sensor.Path.ForcedAndSceneCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPathForcedTest::RunTest(const FString& Parameters)
{
	using ESP = FCamSimConfig::FRenderConfig::ESensorPath;
	FCamSimConfig Cfg = CleanGpuConfig();
	Cfg.SensorModeConfigs[ESensorMode::EO].NETD = 0.02f;
	Cfg.Render.SensorPathMode = ESP::Gpu;
	const FSensorPathDecision D = FSensorPathSelector::Decide(Cfg);
	TestTrue(TEXT("forced gpu"), D.Path == ESensorPipelinePath::Gpu);
	TestTrue(TEXT("ignored effects still listed"), D.Unported.Num() == 1 && D.Reason.Contains(TEXT("ignored")));

	Cfg = CleanGpuConfig();
	Cfg.Render.SensorPathMode = ESP::Legacy;
	TestTrue(TEXT("forced legacy"), FSensorPathSelector::Decide(Cfg).Path == ESensorPipelinePath::Legacy);

	Cfg.Render.SensorPathMode = ESP::Gpu;
	Cfg.Render.ViewSourceMode = FCamSimConfig::FRenderConfig::EViewSource::SceneCapture;
	TestTrue(TEXT("scene_capture is always legacy"), FSensorPathSelector::Decide(Cfg).Path == ESensorPipelinePath::Legacy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPathColorTempTest, "CamSim.Sensor.Path.NeutralValuesAreNotEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPathColorTempTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg = CleanGpuConfig();
	Cfg.SensorModeConfigs[ESensorMode::EO].ColorTemperatureK = 6500.0f;  // neutral
	Cfg.SensorModeConfigs[ESensorMode::EO].bAGCEnabled = true;           // ported in 3B.1
	Cfg.SensorModeConfigs[ESensorMode::IR].AGCLagFrames = 2;             // ported in 3B.1
	TestTrue(TEXT("still gpu"), FSensorPathSelector::Decide(Cfg).Path == ESensorPipelinePath::Gpu);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorMailboxTest, "CamSim.Sensor.Mailbox.NewestWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorMailboxTest::RunTest(const FString& Parameters)
{
	FSensorStatsMailbox Box;
	FSensorHistogram Out;
	TestFalse(TEXT("empty"), Box.TakeLatest(Out));
	FSensorHistogram A; A.Serial = 1; Box.Publish(A);
	FSensorHistogram B; B.Serial = 2; Box.Publish(B);
	TestTrue(TEXT("has one"), Box.TakeLatest(Out));
	TestEqual(TEXT("newest"), Out.Serial, 2u);
	TestFalse(TEXT("consumed"), Box.TakeLatest(Out));
	return true;
}
```

- [ ] **Step 2: Build** — expected: fails (headers missing).

- [ ] **Step 3: Implement.** `SensorStatsMailbox.h` is exactly the class above (plus copyright, `#pragma once`, `CoreMinimal.h`, `Misc/ScopeLock.h`, `SensorFrameParams.h`). `SensorPath.h` declares the **Interfaces** types. `SensorPath.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorPath.h"
#include "Config/CamSimConfig.h"

namespace
{
	/** Enabled effects that 3B.1's GPU path doesn't implement. Shrinks in 3B.2/3B.3. */
	void CollectUnported(const FCamSimConfig& Cfg, TArray<FString>& Out)
	{
		static const TCHAR* ModeNames[] = { TEXT("eo"), TEXT("ir"), TEXT("nvg") };
		for (const TPair<ESensorMode, FSensorModeConfig>& Pair : Cfg.SensorModeConfigs)
		{
			const FSensorModeConfig& M = Pair.Value;
			const TCHAR* Mode = ModeNames[FMath::Clamp(static_cast<int32>(Pair.Key), 0, 2)];
			auto Add = [&](bool bOn, const TCHAR* Key) { if (bOn) Out.Add(FString::Printf(TEXT("%s.%s"), Mode, Key)); };
			Add(M.NETD > 0.0f,                     TEXT("noise_netd"));
			Add(M.FixedPatternNoise > 0.0f,        TEXT("fixed_pattern_noise"));
			Add(M.Vignetting > 0.0f,               TEXT("vignetting"));
			Add(M.bScanLines,                      TEXT("scan_lines"));
			Add(M.IRExtinctionCoeff > 0.0f,        TEXT("ir_extinction_coeff"));
			Add(M.AtmosphericVisibilityM > 0.0f,   TEXT("atmospheric_visibility_m"));
			Add(M.ColorTemperatureK > 0.0f && !FMath::IsNearlyEqual(M.ColorTemperatureK, 6500.0f), TEXT("color_temperature_k"));
			Add(!FMath::IsNearlyEqual(M.Contrast, 1.0f), TEXT("contrast"));
			Add(M.BrightnessBias != 0.0f,          TEXT("brightness_bias"));
			Add(M.BlurRadius > 0,                  TEXT("blur_radius"));
			Add(M.AGCManualLevel >= 0.0f,          TEXT("agc_manual_level"));
			Add(M.QuantizationBits < 8 || M.bQuantizationDither, TEXT("quantization"));
			Add(M.DefectPixelCount > 0,            TEXT("defect_pixel_count"));
			Add(M.GaussianSigma > 0.0f,            TEXT("gaussian_sigma"));
			Add(M.ACBandingAmplitude > 0.0f,       TEXT("ac_banding_amplitude"));
			Add(M.bIRPointerEnabled,               TEXT("ir_pointer_enabled"));
			Add(M.bThermalDriftEnabled,            TEXT("thermal_drift_enabled"));
			Add(M.RollingShutterStrength > 0.0f,   TEXT("rolling_shutter_strength"));
			Add(M.VibrationAmplitude > 0.0f,       TEXT("vibration_amplitude"));
			Add(M.GainJitter > 0.0f || M.OffsetJitter > 0.0f, TEXT("gain_offset_jitter"));
			Add(M.SunGlintIntensity > 0.0f,        TEXT("sun_glint_intensity"));
		}
		if (Cfg.OpticalRealism.bEnabled && Cfg.OpticalRealism.bLensDistortion) Out.Add(TEXT("lens_distortion"));
		if (Cfg.Phase18.bPrecipitation)        Out.Add(TEXT("precipitation"));
		if (Cfg.Phase18.bDynamicIRExtinction)  Out.Add(TEXT("dynamic_ir_extinction"));
		if (Cfg.OverlayConfig.bEnabled)        Out.Add(TEXT("overlay"));
		if (Cfg.LaserDesignator.bEnabled)      Out.Add(TEXT("laser_designator"));
		if (Cfg.Performance.bGpuSensorEffects) Out.Add(TEXT("gpu_sensor_effects"));
	}
}

FSensorPathDecision FSensorPathSelector::Decide(const FCamSimConfig& Cfg)
{
	using ESP = FCamSimConfig::FRenderConfig::ESensorPath;
	FSensorPathDecision D;
	CollectUnported(Cfg, D.Unported);
	const FString List = FString::Join(D.Unported, TEXT(", "));
	if (!Cfg.Render.IsPrimary())
	{
		D.Path = ESensorPipelinePath::Legacy;
		D.Reason = TEXT("sensor path: legacy (view_source is scene_capture)");
	}
	else if (Cfg.Render.SensorPathMode == ESP::Legacy)
	{
		D.Path = ESensorPipelinePath::Legacy;
		D.Reason = TEXT("sensor path: legacy (render.sensor_path = legacy)");
	}
	else if (Cfg.Render.SensorPathMode == ESP::Gpu)
	{
		D.Path = ESensorPipelinePath::Gpu;
		D.Reason = D.Unported.Num() == 0 ? FString(TEXT("sensor path: gpu (forced)"))
			: FString::Printf(TEXT("sensor path: gpu (forced; ignored unported effects: %s)"), *List);
	}
	else
	{
		D.Path = D.Unported.Num() == 0 ? ESensorPipelinePath::Gpu : ESensorPipelinePath::Legacy;
		D.Reason = D.Unported.Num() == 0 ? FString(TEXT("sensor path: gpu"))
			: FString::Printf(TEXT("sensor path: legacy (unported: %s)"), *List);
	}
	return D;
}
```

- [ ] **Step 4: Build and run** `CamSim.Sensor.Path` and `CamSim.Sensor.Mailbox`. Expected: all pass.

- [ ] **Step 5: Commit** — `feat(sensor): sensor path selector and stats mailbox (3B.1)`.

---

### Task 7: NV12 through the encoder (`FSensorFrame`)

**Files:**
- Modify: `Source/CamSimTest/Encoder/IFrameSink.h`, `EncoderThread.h` (`FProcessedFrame`), `EncoderThread.cpp` (l.91, l.115), `VideoEncoder.{h,cpp}` (`EncodeFrame` l.553+), `MultiViewFrameSink.{h,cpp}` (l.93-133, `ApplyDigitalZoom` l.284)
- Modify: `Source/CamSimTest/Camera/CamSimCaptureComponent.cpp` (`SubmitFrameToEncoder` builds `FSensorFrame`), `Tests/EncoderThreadTest.cpp`, `Tests/VideoEncoderRateControlTest.cpp`
- Test: `Source/CamSimTest/Tests/EncoderNv12Test.cpp`

**Interfaces:**
- Consumes: `CamSimNv12::SplitToYuv420p`, `CamSimNv12::NumBytes` (Task 4).
- Produces (in `IFrameSink.h`):

```cpp
enum class ESensorPixelFormat : uint8 { BGRA8, NV12 };

/** One sensor frame for the encoder: BGRA8 (legacy CPU path) or NV12 (GPU path, ROADMAP 3B). */
struct FSensorFrame
{
	ESensorPixelFormat Format = ESensorPixelFormat::BGRA8;
	TArray<FColor>     Bgra;   // W*H
	TArray<uint8>      Nv12;   // W*H*3/2, BT.709 limited range
	bool IsEmpty() const { return Format == ESensorPixelFormat::NV12 ? Nv12.Num() == 0 : Bgra.Num() == 0; }
};
// IFrameSink:
virtual void EncodeFrame(const FSensorFrame& Frame, const FCamSimTelemetry& Telemetry, uint64 FrameIdx) = 0;
// EncoderThread.h:
struct FProcessedFrame { FSensorFrame Frame; FCamSimTelemetry Telemetry; uint64 FrameIndex = 0; };
```

- [ ] **Step 1: Write the failing round-trip test** — `Tests/EncoderNv12Test.cpp`. It encodes 30 NV12 frames with libx264 to a `.ts` recording (the pattern `VideoEncoderRateControlTest` uses), decodes the last frame with libavcodec and checks luma PSNR:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Config/CamSimConfig.h"
#include "Encoder/VideoEncoder.h"
#include "Encoder/Nv12.h"

extern "C"
{
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

namespace
{
	/** Decode the last video frame of a .ts file; returns its Y plane (W*H) or empty. */
	TArray<uint8> DecodeLastLuma(const FString& Path, int32 W, int32 H)
	{
		TArray<uint8> Y;
		AVFormatContext* Fmt = nullptr;
		if (avformat_open_input(&Fmt, TCHAR_TO_UTF8(*Path), nullptr, nullptr) < 0) return Y;
		avformat_find_stream_info(Fmt, nullptr);
		const int Stream = av_find_best_stream(Fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		const AVCodec* Codec = avcodec_find_decoder(Fmt->streams[Stream]->codecpar->codec_id);
		AVCodecContext* Ctx = avcodec_alloc_context3(Codec);
		avcodec_parameters_to_context(Ctx, Fmt->streams[Stream]->codecpar);
		avcodec_open2(Ctx, Codec, nullptr);
		AVPacket* Pkt = av_packet_alloc();
		AVFrame* Frame = av_frame_alloc();
		auto Drain = [&]()
		{
			while (avcodec_receive_frame(Ctx, Frame) == 0 && Frame->width == W && Frame->height == H)
			{
				Y.SetNumUninitialized(W * H);
				for (int32 R = 0; R < H; ++R) FMemory::Memcpy(Y.GetData() + R * W, Frame->data[0] + R * Frame->linesize[0], W);
			}
		};
		while (av_read_frame(Fmt, Pkt) >= 0)
		{
			if (Pkt->stream_index == Stream && avcodec_send_packet(Ctx, Pkt) == 0) Drain();
			av_packet_unref(Pkt);
		}
		avcodec_send_packet(Ctx, nullptr);
		Drain();
		av_frame_free(&Frame); av_packet_free(&Pkt); avcodec_free_context(&Ctx); avformat_close_input(&Fmt);
		return Y;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEncoderNv12RoundTripTest, "CamSim.Encoder.Nv12.RoundTripPsnr",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEncoderNv12RoundTripTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 640, H = 360, Frames = 30;
	FCamSimConfig Config;
	Config.CaptureWidth = W; Config.CaptureHeight = H; Config.FrameRate = 30.0f;
	Config.VideoBitrate = 8'000'000;
	Config.EncoderPreference = TEXT("libx264");
	Config.MulticastAddr = TEXT("127.0.0.1"); Config.MulticastPort = 49998;
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("nv12_roundtrip.ts"));
	IFileManager::Get().Delete(*Path);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	Config.Recording.VideoRecordPath = Path;

	FSensorFrame F;
	F.Format = ESensorPixelFormat::NV12;
	F.Nv12.SetNumUninitialized(CamSimNv12::NumBytes(W, H));
	for (int32 R = 0; R < H; ++R) for (int32 X = 0; X < W; ++X) F.Nv12[R * W + X] = static_cast<uint8>(16 + ((X + R) * 219) / (W + H));
	for (int32 I = W * H; I < F.Nv12.Num(); ++I) F.Nv12[I] = (I & 1) ? 150 : 110;
	{
		FVideoEncoder Encoder(Config);
		if (!TestTrue(TEXT("opened"), Encoder.Open())) return false;
		FCamSimTelemetry T;
		for (int32 I = 0; I < Frames; ++I) Encoder.EncodeFrame(F, T, I);
		Encoder.Close();
	}
	const TArray<uint8> Y = DecodeLastLuma(Path, W, H);
	IFileManager::Get().Delete(*Path);
	if (!TestEqual(TEXT("decoded a frame"), Y.Num(), W * H)) return false;
	double Mse = 0.0;
	for (int32 I = 0; I < W * H; ++I) { const double D = double(Y[I]) - double(F.Nv12[I]); Mse += D * D; }
	Mse /= (W * H);
	const double Psnr = Mse > 0.0 ? 10.0 * FMath::LogX(10.0, 255.0 * 255.0 / Mse) : 99.0;
	TestTrue(FString::Printf(TEXT("luma PSNR %.1f dB >= 40"), Psnr), Psnr >= 40.0);
	return true;
}
```

Use the real config field names — check `FCamSimConfig` for the encoder-choice field (search for `CAMSIM_ENCODER`) and `Recording.VideoRecordPath`; adjust the two lines if they differ.

- [ ] **Step 2: Build** — expected: fails (`FSensorFrame` undefined).

- [ ] **Step 3: Change the interface and its users.**
  - `IFrameSink.h`: add the enum/struct; replace the `EncodeFrame` signature; update its doc comment ("Encode one sensor frame (BGRA8 or NV12)").
  - `EncoderThread.h`: `FProcessedFrame` as in **Interfaces**. `EncoderThread.cpp` l.91 and l.115: `Encoder->EncodeFrame(Frame.Frame, Frame.Telemetry, Frame.FrameIndex);` / `Remaining.Frame`.
  - `CamSimCaptureComponent.cpp` `SubmitFrameToEncoder`: `Frame.Frame.Format = ESensorPixelFormat::BGRA8; Frame.Frame.Bgra = MoveTemp(Pixels);` (Task 8 adds the NV12 branch).
  - `Tests/EncoderThreadTest.cpp` l.23: `virtual void EncodeFrame(const FSensorFrame&, const FCamSimTelemetry&, uint64) override`.
  - `Tests/VideoEncoderRateControlTest.cpp` l.122: wrap `Pixels` — build an `FSensorFrame Frame; Frame.Bgra = Pixels;` before the loop body's `EncodeFrame(Frame, T, Frame)` (rename the loop counter to avoid the clash).
- [ ] **Step 4: `FVideoEncoder::EncodeFrame(const FSensorFrame& Frame, …)`** — at the top, replace `PixelData.Num() == 0` with `Frame.IsEmpty()`. Insert before the grayscale branch:

```cpp
	av_frame_make_writable(YuvFrame);
	if (Frame.Format == ESensorPixelFormat::NV12)
	{
		// GPU sensor path (ROADMAP 3B): already BT.709 limited-range YUV; just de-interleave.
		if (Frame.Nv12.Num() != CamSimNv12::NumBytes(Config.CaptureWidth, Config.CaptureHeight))
		{
			UE_LOG(LogCamSim, Warning, TEXT("FVideoEncoder: NV12 frame %llu has %d bytes, expected %d — skipped"),
				FrameIdx, Frame.Nv12.Num(), CamSimNv12::NumBytes(Config.CaptureWidth, Config.CaptureHeight));
			return;
		}
		CamSimNv12::SplitToYuv420p(Frame.Nv12.GetData(), Config.CaptureWidth, Config.CaptureHeight,
			YuvFrame->data[0], YuvFrame->linesize[0], YuvFrame->data[1], YuvFrame->linesize[1],
			YuvFrame->data[2], YuvFrame->linesize[2]);
	}
	else
	{
		const TArray<FColor>& PixelData = Frame.Bgra;
		… existing grayscale / sws_scale code, unchanged …
	}
```

Move the existing `av_frame_make_writable` call up (it now precedes both branches), and keep the frame-0 diagnostic only in the BGRA branch (it indexes `PixelData`). Add `#include "Encoder/Nv12.h"`.
- [ ] **Step 5: `FMultiViewFrameSink`** — change `EncodeFrame` to take `const FSensorFrame&`. For each view needing zoom: BGRA → existing `ApplyDigitalZoom(Frame.Bgra, …, View.ZoomedScratch.Bgra)`; NV12 → new `ApplyDigitalZoomNv12(Frame.Nv12, Width, Height, SourceHFov, TargetHFov, View.ZoomedScratch.Nv12)`; change `ZoomedScratch` to an `FSensorFrame` and set its `Format`. Implement `ApplyDigitalZoomNv12` next to `ApplyDigitalZoom`, using the same crop/nearest-neighbour mapping that function uses (read l.284-316 and mirror its `SrcX`/`SrcY` computation): for Y use the full-res mapping; for UV sample at `(SrcY/2, (SrcX/2)*2)` for each even destination pair. Same identity short-cut (`memcpy`) when no zoom.
- [ ] **Step 6: Build and run** `CamSim.Encoder` (all encoder tests, including `Nv12.RoundTripPsnr`, `Thread.*`, rate control) under NullRHI. Expected: all pass.
- [ ] **Step 7: Commit** — `feat(encoder): NV12 sensor frames through the encoder (3B.1)`.

---

### Task 8: Wire the GPU path into the frame grab and capture component

**Files:**
- Create: `Source/CamSimTest/Camera/SensorGpuTimer.{h,cpp}`
- Modify: `Source/CamSimTest/Camera/CamSimFrameGrabExtension.{h,cpp}`, `Source/CamSimTest/Camera/CamSimCaptureComponent.{h,cpp}`, `Source/CamSimTest/Camera/CamSimCamera.{h,cpp}`, `Source/CamSimTest/Subsystem/CamSimSubsystem.cpp`

**Interfaces:**
- Consumes: everything above (`AddSensorPasses`, `FSensorController`, `FSensorPathSelector`, `FSensorStatsMailbox`, `FSensorFrame`, `CamSimNv12::ToBgra`).
- Produces:
  - `FCamSimFrameGrabExtension::EnableGpuSensor_GameThread(FIntPoint CaptureSize, FSensorStatsMailbox* Mailbox, FSensorGpuTimer* Timer)`, `SetParams_RenderThread(const FSensorFrameParams&)`, and `PushRequest_RenderThread(const FFrameGrabRequest&, FRHITexture* Target, FRHIGPUTextureReadback*, FRHIGPUBufferReadback* Nv12Readback, TAtomic<uint32>* Grabbed)` (texture args null in GPU mode, NV12 arg null in legacy mode).
  - `UCamSimCaptureComponent::UpdateSensorParams(ESensorMode Mode, uint8 Polarity, bool bCameraCut, const FCamSimConfig& Cfg)` (replaces `UpdateGpuSensorParams`), `GetSensorPath()`, `GetSensorGpuMs()`, `GetSensorGainEv()`, `GetSceneMedianLog2()`.
  - `ACamSimCamera::GetSensorPath() const`.
  - `FFrameDropStats::SensorStatsStale` (not counted in `Total()`).

This task is integration: its correctness is shown by the live checks in Step 9 plus the automated tests already written. Keep the legacy code path untouched — every change is behind `bGpuSensor`.

- [ ] **Step 1: `SensorGpuTimer`** — render-thread helper measuring GPU time between two RDG passes with timestamp queries:

```cpp
// SensorGpuTimer.h
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "RHIResources.h"
#include "RenderGraphDefinitions.h"

/** GPU time of the sensor graph from timestamp queries (ROADMAP 3B bench). Render thread, except GetLatestMs. */
class FSensorGpuTimer
{
public:
	void Begin(FRDGBuilder& GraphBuilder);
	void End(FRDGBuilder& GraphBuilder);
	/** Newest completed measurement in ms; -1 when timestamps are unsupported or none finished yet. Any thread. */
	float GetLatestMs() const { return LatestMs.Load(EMemoryOrder::Relaxed); }
private:
	static constexpr int32 NumFrames = 4;
	struct FPair { FRenderQueryRHIRef Start, Stop; bool bPending = false; };
	FPair Pairs[NumFrames];
	int32 Current = 0;
	TAtomic<float> LatestMs { -1.0f };
	void Harvest(FRHICommandListImmediate& RHICmdList);
};
```

```cpp
// SensorGpuTimer.cpp
// Copyright CamSim Contributors. All Rights Reserved.
#include "Camera/SensorGpuTimer.h"
#include "RenderGraphBuilder.h"
#include "RHICommandList.h"

void FSensorGpuTimer::Harvest(FRHICommandListImmediate& RHICmdList)
{
	for (FPair& P : Pairs)
	{
		if (!P.bPending) continue;
		uint64 StartUs = 0, StopUs = 0;
		if (RHIGetRenderQueryResult(P.Start, StartUs, false) && RHIGetRenderQueryResult(P.Stop, StopUs, false))
		{
			P.bPending = false;
			if (StopUs >= StartUs) LatestMs.Store((StopUs - StartUs) / 1000.0f, EMemoryOrder::Relaxed);
		}
	}
}

void FSensorGpuTimer::Begin(FRDGBuilder& GraphBuilder)
{
	if (!GSupportsTimestampRenderQueries) return;
	Current = (Current + 1) % NumFrames;
	GraphBuilder.AddPass(RDG_EVENT_NAME("CamSimSensorTimerBegin"), ERDGPassFlags::NeverCull,
		[this, Index = Current](FRHICommandListImmediate& RHICmdList)
	{
		Harvest(RHICmdList);
		FPair& P = Pairs[Index];
		if (P.bPending) return;  // still unread after NumFrames: skip this measurement
		if (!P.Start) { P.Start = RHICreateRenderQuery(RQT_AbsoluteTime); P.Stop = RHICreateRenderQuery(RQT_AbsoluteTime); }
		RHICmdList.EndRenderQuery(P.Start);
	});
}

void FSensorGpuTimer::End(FRDGBuilder& GraphBuilder)
{
	if (!GSupportsTimestampRenderQueries) return;
	GraphBuilder.AddPass(RDG_EVENT_NAME("CamSimSensorTimerEnd"), ERDGPassFlags::NeverCull,
		[this, Index = Current](FRHICommandListImmediate& RHICmdList)
	{
		FPair& P = Pairs[Index];
		if (!P.Stop || P.bPending) return;
		RHICmdList.EndRenderQuery(P.Stop);
		P.bPending = true;
	});
}
```

(Check `RHICreateRenderQuery` / `RHIGetRenderQueryResult` names in UE 5.8's `DynamicRHI.h`/`RHICommandList.h`; if 5.8 routes these through `FRHICommandListImmediate` or a query pool, use that form — the behaviour is the same. `ERDGPassFlags::NeverCull` must be combined with a pass type; use `ERDGPassFlags::None | ERDGPassFlags::NeverCull` if the plain flag is rejected.)

- [ ] **Step 2: Extension — subscription and sensor run.** In `CamSimFrameGrabExtension.h` add includes/forward declarations for `FRHIGPUBufferReadback`, `FSensorStatsMailbox`, `FSensorGpuTimer`, `SensorFrameParams.h`; add:

```cpp
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

	/** Game thread, once: run the GPU sensor model in place of the tonemapper (ROADMAP 3B). */
	void EnableGpuSensor_GameThread(FIntPoint InCaptureSize, FSensorStatsMailbox* InMailbox, FSensorGpuTimer* InTimer);
	/** Render thread: parameters for the next frames the graph runs. */
	void SetParams_RenderThread(const FSensorFrameParams& P) { Params = P; }

private:
	FScreenPassTexture RunSensor_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
	void ReadStats_RenderThread(FRDGBuilder& GraphBuilder, FRDGBufferRef Histogram, uint32 Serial);

	TAtomic<bool>        bGpuSensor { false };
	FIntPoint            CaptureSize = FIntPoint::ZeroValue;   // set before bGpuSensor
	FSensorStatsMailbox* Mailbox = nullptr;
	FSensorGpuTimer*     Timer   = nullptr;
	FSensorFrameParams   Params;                               // render thread
	struct FStatsSlot { TUniquePtr<FRHIGPUBufferReadback> Readback; uint32 Serial = 0; bool bPending = false; };
	FStatsSlot           StatsRing[4];                         // render thread
	int32                NextStatsSlot = 0;
```

Extend `FTargets` with `FRHIGPUBufferReadback* Nv12Readback = nullptr;` and give `PushRequest_RenderThread` the extra `Nv12Readback` parameter (store it). In `PostRenderViewFamily_RenderThread`, return immediately when `bGpuSensor.Load()` (the grab happens in `RunSensor_RenderThread`).

In the `.cpp`:

```cpp
void FCamSimFrameGrabExtension::EnableGpuSensor_GameThread(FIntPoint InCaptureSize, FSensorStatsMailbox* InMailbox, FSensorGpuTimer* InTimer)
{
	CaptureSize = InCaptureSize;
	Mailbox = InMailbox;
	Timer = InTimer;
	bGpuSensor.Store(true);
}

void FCamSimFrameGrabExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::ReplacingTonemapper && bIsPassEnabled && bGpuSensor.Load())
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FCamSimFrameGrabExtension::RunSensor_RenderThread));
	}
}

void FCamSimFrameGrabExtension::ReadStats_RenderThread(FRDGBuilder& GraphBuilder, FRDGBufferRef Histogram, uint32 Serial)
{
	// Harvest finished readbacks (oldest first), publish the newest, then queue this frame's.
	constexpr uint32 Bytes = FSensorHistogram::NumBins * sizeof(uint32);
	for (int32 K = 1; K <= UE_ARRAY_COUNT(StatsRing); ++K)
	{
		FStatsSlot& S = StatsRing[(NextStatsSlot + K) % UE_ARRAY_COUNT(StatsRing)];
		if (S.bPending && S.Readback->IsReady())
		{
			FSensorHistogram H;
			FMemory::Memcpy(H.Bins.GetData(), S.Readback->Lock(Bytes), Bytes);
			S.Readback->Unlock();
			H.Serial = S.Serial;
			S.bPending = false;
			if (Mailbox) Mailbox->Publish(H);
		}
	}
	FStatsSlot& Slot = StatsRing[NextStatsSlot];
	if (Slot.bPending) return;  // ring full (GPU far behind): skip this frame's stats
	if (!Slot.Readback) Slot.Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("CamSimSensorStats"));
	Slot.Serial = Serial;
	Slot.bPending = true;
	NextStatsSlot = (NextStatsSlot + 1) % UE_ARRAY_COUNT(StatsRing);
	AddEnqueueCopyPass(GraphBuilder, Slot.Readback.Get(), Histogram, Bytes);
}

FScreenPassTexture FCamSimFrameGrabExtension::RunSensor_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder,
		Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	const FScreenPassTextureSlice BloomSlice = Inputs.GetInput(EPostProcessMaterialInput::CombinedBloom);

	FSensorGraphInputs In;
	In.SceneColor = SceneColor.Texture;
	In.SceneViewRect = SceneColor.ViewRect;
	if (BloomSlice.IsValid())
	{
		const FScreenPassTexture Bloom = FScreenPassTexture::CopyFromSlice(GraphBuilder, BloomSlice);
		In.Bloom = Bloom.Texture;
		In.BloomViewRect = Bloom.ViewRect;
	}
	In.ViewUniformBuffer = View.ViewUniformBuffer.GetReference();
	In.OutputSize = CaptureSize;

	if (!bWarnedViewSize && SceneColor.ViewRect.Height() > 0
		&& !FMath::IsNearlyEqual(double(SceneColor.ViewRect.Width()) / SceneColor.ViewRect.Height(), double(CaptureSize.X) / CaptureSize.Y, 0.005))
	{
		bWarnedViewSize = true;
		UE_LOG(LogCamSim, Warning, TEXT("SensorGraph: view is %dx%d but the capture is %dx%d; the image is stretched and its vertical FOV won't match the KLV"),
			SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(), CaptureSize.X, CaptureSize.Y);
	}

	if (Timer) Timer->Begin(GraphBuilder);
	const FSensorGraphOutputs Out = AddSensorPasses(GraphBuilder, In, Params);
	if (Timer) Timer->End(GraphBuilder);
	ReadStats_RenderThread(GraphBuilder, Out.Histogram, Params.Serial);

	FFrameGrabRequest Req;
	if (Requests.PopLatest(Req))
	{
		const FTargets* T = TargetsBySlot.Find(Req.TargetIndex);
		if (T && T->Nv12Readback && T->GrabbedGeneration)
		{
			FRHIGPUBufferReadback* Readback = T->Nv12Readback;
			TAtomic<uint32>* Grabbed = T->GrabbedGeneration;
			const uint32 Gen = Req.Generation;
			const uint32 Bytes = Out.Nv12Bytes;
			AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CamSimNv12Readback"), Out.Nv12,
				[Readback, Buffer = Out.Nv12, Bytes, Grabbed, Gen](FRHICommandListImmediate& RHICmdList)
			{
				Readback->EnqueueCopy(RHICmdList, Buffer->GetRHI(), Bytes);
				Grabbed->Store(Gen, EMemoryOrder::SequentiallyConsistent);
			});
		}
	}

	// The viewport shows exactly what is streamed (NVG tinted).
	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, ERenderTargetLoadAction::ENoAction, TEXT("CamSimSensorDisplay"));
	}
	AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(View), Out.SensorRgb, Output.Texture,
		FIntPoint::ZeroValue, CaptureSize, Output.ViewRect.Min, Output.ViewRect.Size());
	return FScreenPassTexture(Output);
}
```

Add includes: `SensorGraph.h`, `Sensor/SensorStatsMailbox.h`, `Camera/SensorGpuTimer.h`, `PostProcess/PostProcessMaterialInputs.h`, `ScreenPass.h`. `EPostProcessMaterialInput` / `FPostProcessMaterialInputs` are in the Renderer's public headers (already a module dependency). The texture grab path uses `Grabbed->Store` inside the readback lambda the same way; keep that pattern. The `AddReadbackBufferPass` lambda runs on the render thread inline, which preserves the "generation published only once the copy is queued" invariant (see the existing comment above `AddReadbackTexturePass`).

- [ ] **Step 3: Capture component — path selection and resources.** In `CamSimCaptureComponent.h`: add includes `Sensor/SensorPath.h`, `Sensor/SensorController.h`, `Sensor/SensorStatsMailbox.h`, `Camera/SensorGpuTimer.h`; members:

```cpp
	/** ROADMAP 3B: which sensor pipeline this session runs (fixed at Initialize). */
	FSensorPathDecision SensorPath;
	bool bGpuSensor = false;
	TArray<TUniquePtr<FRHIGPUBufferReadback>> Nv12ReadbackPool;
	FSensorController   SensorController;
	FSensorStatsMailbox StatsMailbox;
	FSensorGpuTimer     GpuTimer;
	uint32 ParamsSerial = 0;          // game thread
	double LastSensorUpdateSimSec = -1.0;
```

and in `FSlot` add `TArray<uint8> Nv12;`. Add `TAtomic<int32> SensorStatsStale { 0 };` to `FFrameDropStats` (not in `Total()`), and the public getters from **Interfaces** (`GetSensorGpuMs()` returns `GpuTimer.GetLatestMs()`).

In `Initialize`, right after `bPrimaryView = Cfg.Render.IsPrimary();`:

```cpp
	SensorPath = FSensorPathSelector::Decide(Cfg);
	bGpuSensor = SensorPath.Path == ESensorPipelinePath::Gpu;
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: %s"), *SensorPath.Reason);
	if (bGpuSensor)
	{
		Nv12ReadbackPool.Reset();
		for (int32 Idx = 0; Idx < FReadbackRing::NumSlots; ++Idx)
		{
			Nv12ReadbackPool.Add(MakeUnique<FRHIGPUBufferReadback>(*FString::Printf(TEXT("CamSimNv12Readback_%d"), Idx)));
		}
	}
```

In `EnsureGrabExtension`, after creating the extension: `if (bGpuSensor) GrabExtension->EnableGpuSensor_GameThread(FIntPoint(CfgW, CfgH), &StatsMailbox, &GpuTimer);` — take the capture size from `Subsystem->GetConfig()`. In `Shutdown`, add `Nv12ReadbackPool.Reset();` after `ColorReadbackPool.Reset();` (the existing `FlushRenderingCommands()` already precedes it).

- [ ] **Step 4: UE manual exposure in the GPU path.** In `ApplyRenderSettings`, wrap the existing "Auto-exposure stays on" block:

```cpp
	if (bGpuSensor)
	{
		// ROADMAP 3B: the sensor owns exposure. UE runs manual so its eye adaptation
		// never fights the sensor AE; UpdateSensorParams sets the bias each tick only
		// to keep scene colour in fp16 range (the graph divides PreExposure back out).
		PP.bOverride_AutoExposureMethod = true;
		PP.AutoExposureMethod = AEM_Manual;
		PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		PP.AutoExposureApplyPhysicalCameraExposure = false;
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = 0.0f;
	}
	else
	{
		… existing AutoExposureBias block, unchanged …
	}
```

`ApplyRenderSettings` runs after `bGpuSensor` is set (Step 3 sets it before the call) — verify the order in `Initialize`.

- [ ] **Step 5: `UpdateSensorParams`** — replaces `UpdateGpuSensorParams` (delete that function and its declaration; the material path is deleted in 3B.4, but its per-tick MPC update is dead anyway and its call site is replaced here):

```cpp
void UCamSimCaptureComponent::UpdateSensorParams(ESensorMode Mode, uint8 Polarity, bool bCameraCut, const FCamSimConfig& Cfg)
{
	if (!bGpuSensor || !GrabExtension) return;

	const double NowSimSec = static_cast<double>(FSimClock::Get().NowMicros()) * 1e-6;
	const double Dt = LastSensorUpdateSimSec < 0.0 ? 0.0 : NowSimSec - LastSensorUpdateSimSec;
	LastSensorUpdateSimSec = NowSimSec;

	FSensorHistogram Hist;
	const bool bHasHist = StatsMailbox.TakeLatest(Hist);

	FSensorControllerInput In;
	In.Mode        = static_cast<ESensorGraphMode>(FMath::Clamp(static_cast<int32>(Mode), 0, 2));
	In.bBlackHot   = Polarity != 0;
	In.bCameraCut  = bCameraCut;
	In.DeltaSimSec = Dt;
	In.NewHistogram = bHasHist ? &Hist : nullptr;
	In.ExposureCompensationEv = Cfg.Render.ExposureCompensationEV;
	In.Serial      = ++ParamsSerial;
	const FSensorModeConfig* ModeCfg = Cfg.SensorModeConfigs.Find(Mode);
	const uint32 StaleBefore = SensorController.GetStaleEpisodes();
	const FSensorFrameParams Params = SensorController.Update(In, ModeCfg ? *ModeCfg : FSensorModeConfig());
	if (SensorController.GetStaleEpisodes() != StaleBefore && bTrackFrameDrops) FrameDropStats.SensorStatsStale++;

	// Keep UE's pre-exposure near the sensor gain so scene colour stays in fp16 range.
	Sensor->PostProcessSettings.AutoExposureBias = SensorController.GetGainEv() + UeExposureOffsetEv;

	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> Ext = GrabExtension;
	ENQUEUE_RENDER_COMMAND(CamSimSensorParams)([Ext, Params](FRHICommandListImmediate&)
	{
		Ext->SetParams_RenderThread(Params);
	});
}
```

Declare `static constexpr float UeExposureOffsetEv = 0.0f;` in the header with the comment "calibrated in Task 11 so View.PreExposure ≈ sensor gain". Add `#include "Time/SimClock.h"`.

- [ ] **Step 6: Capture and poll for NV12.** In `Capture()`, the `bPrimaryView` branch passes both readbacks:

```cpp
		FRHIGPUBufferReadback* Nv12Readback = (bGpuSensor && Nv12ReadbackPool.IsValidIndex(Slot)) ? Nv12ReadbackPool[Slot].Get() : nullptr;
		…
		Ext->PushRequest_RenderThread({ FrameIdx, Gen, Slot },
			bGpuSensor ? nullptr : Resource->GetRenderTargetTexture(), bGpuSensor ? nullptr : Readback, Nv12Readback, Grabbed);
```

(capture `Nv12Readback` and `bGpuSensor` in the lambda; keep the `Resource` null-check only for the legacy branch.)

In `EnqueuePoll`, capture `FRHIGPUBufferReadback* Nv12Readback = bGpuSensor ? Nv12ReadbackPool[Slot].Get() : nullptr;` and `const uint32 Nv12Bytes = CamSimNv12::NumBytes(CaptureW, CaptureH);`. Pass `[Nv12Readback]() { return Nv12Readback ? Nv12Readback->IsReady() : (Readback && Readback->IsReady()); }` as the fence predicate. After the ready-streak check, branch before the texture `Lock`:

```cpp
		if (Nv12Readback)
		{
			const void* Raw = Nv12Readback->Lock(Nv12Bytes);
			if (!Raw) { RingPtr->MarkFailed(Slot); return; }   // null: do NOT Unlock
			S.Nv12.SetNumUninitialized(Nv12Bytes);
			FMemory::Memcpy(S.Nv12.GetData(), Raw, Nv12Bytes);
			Nv12Readback->Unlock();
			S.Pixels.Reset();
			// depth handling below is shared; fall through to it
		}
		else
		{
			… existing texture Lock / format check / CamSimConvertReadbackPixels / Unlock …
		}
```

Restructure so the depth block and `MarkComplete` run for both branches (move the texture block into the `else`; keep its early-return failure paths).

In `Poll()`, replace the snapshot and submit lines with:

```cpp
		if (FCamSimSnapshotService* Snap = Subsystem ? Subsystem->GetSnapshotService() : nullptr)
		{
			if (Snap->WantsFrame()) OfferSnapshot(*Snap, S);
		}
		if (FCamSimSnapshotService* SensorSnap = Subsystem ? Subsystem->GetSensorSnapshotService() : nullptr)
		{
			if (SensorSnap->WantsFrame()) OfferSnapshot(*SensorSnap, S);
		}
		bSensorBusy = true;
		SubmitFrameToEncoder(MoveTemp(S.Pixels), MoveTemp(S.Nv12), S.Telemetry, FrameIdx, MoveTemp(S.Depth));
		S.Pixels.Reset();
		S.Nv12.Reset();
		S.Depth.Reset();
```

with a private helper:

```cpp
void UCamSimCaptureComponent::OfferSnapshot(FCamSimSnapshotService& Snap, const FSlot& S) const
{
	const FCamSimConfig& C = Subsystem->GetConfig();
	if (S.Nv12.Num() > 0)
	{
		TArray<FColor> Bgra;
		CamSimNv12::ToBgra(S.Nv12.GetData(), C.CaptureWidth, C.CaptureHeight, Bgra);
		Snap.OfferFrame(Bgra, C.CaptureWidth, C.CaptureHeight);
	}
	else
	{
		Snap.OfferFrame(S.Pixels, C.CaptureWidth, C.CaptureHeight);
	}
}
```

(For the legacy path, `/snapshot/sensor` therefore returns the pre-sensor frame, as before 3B; documented in Task 9.)

`SubmitFrameToEncoder` gains `TArray<uint8> Nv12`; inside the task: `if (Nv12.Num() > 0) { Frame.Frame.Format = ESensorPixelFormat::NV12; Frame.Frame.Nv12 = MoveTemp(Nv12); } else { if (FX) FX->Process(…); Frame.Frame.Bgra = MoveTemp(Pixels); }` — the CPU sensor model runs only for BGRA frames. Latency marks stay around the (now conditional) `Process` call.

- [ ] **Step 7: Camera tick, reporting.** In `ACamSimCamera::Tick` replace `CaptureComp->UpdateGpuSensorParams(SensorComp->GetMode(), Cfg);` with `CaptureComp->UpdateSensorParams(SensorComp->GetMode(), Telemetry.Get().SensorPolarity, bCameraCutThisFrame, Cfg);` (`UpdateCameraCut()` already ran above it this tick). Add `ESensorPipelinePath GetSensorPath() const { return CaptureComp->GetSensorPath(); }` to `ACamSimCamera`. In `CamSimSubsystem.cpp`:
  - the `/metrics` lambda (~l.461): append
    ```cpp
    if (const ACamSimCamera* Cam = GetCamera())
    {
        Body += TEXT("# HELP camsim_sensor_path Sensor pipeline in use (ROADMAP 3B).\n# TYPE camsim_sensor_path gauge\n");
        Body += FString::Printf(TEXT("camsim_sensor_path{path=\"%s\"} 1\n"), FSensorPathSelector::ToString(Cam->GetSensorPath()));
    }
    ```
  - the health JSON (~l.885): after the `terrain_ready` field, append `,"sensor_path":"gpu|legacy"` via `FString::Printf(TEXT(",\"sensor_path\":\"%s\""), …)` when a camera exists; inside the `frame_drops` object add `"sensor_stats_stale":%d`.
- [ ] **Step 8: Build and run the whole NullRHI suite** (`CamSim`). Expected: all pass (the legacy path is what NullRHI exercises).
- [ ] **Step 9: Live check (GPU path).** Run:

```bash
CAMSIM_RENDER_SENSOR_PATH=gpu CAMSIM_MULTICAST_ADDR=127.0.0.1 scripts/run.sh --headless --local --detach
python3 scripts/send_cigi_test.py --circle &   # host traffic (terrain gate, /ready)
sleep 60; curl -s localhost:8080/metrics | grep camsim_sensor_path   # expect path="gpu"
ffprobe -v error -show_streams -i "udp://127.0.0.1:5004?timeout=5000000" | grep -E "codec_name|width|avg_frame_rate"
curl -s -o /tmp/claude-sensor.png localhost:8080/snapshot/sensor   # after Task 9; skip until then
grep -E "sensor path|SensorGraph|stretched" ~/Library/Logs/CamSimTest/CamSimTest.log
scripts/stop.sh; kill %1
```

Expected: log line `sensor path: gpu (forced; ignored unported effects: …)`; an h264 stream at 1280x720 ~30 fps; no "stretched" warning at the default window. Then repeat with `CAMSIM_ENCODER=libx264` and `CAMSIM_CAPTURE_WIDTH=1920 CAMSIM_CAPTURE_HEIGHT=1080` (use the real env var names from `docs/configuration.md`) for 60 s and confirm in `camsim_health.json` that `frame_drops.sensor_stats_stale` stays 0 while `encoder_busy` may rise (Review Focus 5). Then run once with no override (`auto`) and confirm `path="legacy"` and an unchanged stream.
- [ ] **Step 10: Commit** — `feat(render): GPU sensor model replaces the tonemapper in the primary view (3B.1)`.

---

### Task 9: `/snapshot/sensor` and frame-stats keys

**Files:**
- Modify: `Source/CamSimTest/Health/CamSimHealthServer.{h,cpp}` (`BindSnapshotRoute` gains a path), `Source/CamSimTest/Subsystem/CamSimSubsystem.{h,cpp}` (second service), `Source/CamSimTest/Camera/CamSimFrameStats.{h,cpp}`, `Source/CamSimTest/Camera/CamSimCamera.cpp` (`RecordFrameStats`)
- Modify tests: `Tests/SnapshotEndpointTest.cpp`, `Tests/RenderPathTest.cpp` (`FrameStats.RowFormat`)

**Interfaces:**
- Produces: `FCamSimHealthServer::BindSnapshotRoute(const FString& Path, TFunction<void(FHttpResultCallback)> Handler)`; `UCamSimSubsystem::GetSensorSnapshotService() const`; `FCamSimFrameStatsSample::{SensorGpuMs = -1.0, SensorGainEv = 0.0, SceneMedianLog2 = 0.0}` → JSON keys `sensor_gpu_ms`, `sensor_gain_ev`, `scene_median_log2`.

- [ ] **Step 1: Failing tests.** In `RenderPathTest.cpp` `FFrameStatsRowTest`, set `S.SensorGpuMs = 1.25; S.SensorGainEv = -13.5; S.SceneMedianLog2 = 11.0;` and assert `TestEqual(TEXT("sensor_gpu_ms"), Obj->GetNumberField(TEXT("sensor_gpu_ms")), 1.25);` plus the other two keys. In `SnapshotEndpointTest.cpp` add a test `CamSim.Health.Snapshot.SensorRoute` copying the existing end-to-end test (the one at l.134 that binds, requests, offers a frame and expects a PNG) but binding `TEXT("/snapshot/sensor")` and requesting that URL; and change the two existing `BindSnapshotRoute(` calls to pass `TEXT("/snapshot")` first.
- [ ] **Step 2: Build** — fails (new fields / signature).
- [ ] **Step 3: Implement.**
  - `CamSimFrameStats.h`: add the three fields with comments ("GPU time of the sensor graph, -1 when unavailable (ROADMAP 3B)", "log2 sensor gain", "log2 histogram median of the detector signal"). `CamSimFrameStats.cpp` `CamSimFormatFrameStatsRow`: append `,"sensor_gpu_ms":%.3f,"sensor_gain_ev":%.3f,"scene_median_log2":%.3f` in the same `Printf` style as the existing keys.
  - `ACamSimCamera::RecordFrameStats`: `S.SensorGpuMs = CaptureComp->GetSensorGpuMs(); S.SensorGainEv = CaptureComp->GetSensorGainEv(); S.SceneMedianLog2 = CaptureComp->GetSceneMedianLog2();`.
  - `CamSimHealthServer`: replace the single `SnapshotHandler` with `TMap<FString, TFunction<void(FHttpResultCallback)>> SnapshotHandlers`; `BindSnapshotRoute(Path, Handler)` stores it and, if the router exists and the path wasn't bound before, binds `FHttpPath(Path)` with the same lambda body as `BindSnapshotHandler` (which now loops over the map when routes are (re)bound on listen). Log `"/snapshot" enabled` per path.
  - `CamSimSubsystem.cpp` (~l.542): create `SensorSnapshotService` next to `SnapshotService` and bind it at `TEXT("/snapshot/sensor")`; tick it where `SnapshotService->Tick` is called; reset it where `SnapshotService.Reset()` is; add the getter.
- [ ] **Step 4: Build and run** `CamSim.Health` and `CamSim.Render`. Expected: all pass.
- [ ] **Step 5: Docs** — `docs/configuration.md` (or wherever `/snapshot` is documented; `grep -rn "/snapshot" docs CLAUDE.md`): document `/snapshot/sensor` ("the encoded sensor image as PNG; legacy path: same as `/snapshot`") and the three frame-stats keys.
- [ ] **Step 6: Commit** — `feat(health): /snapshot/sensor and sensor frame-stats keys (3B.1)`.

---

### Task 10: Bench — sensor path, IR/NVG/night shots, sensor timing

**Files:**
- Modify: `scripts/bench/scenario.py`, `scripts/bench/run_bench.py`, `scripts/bench/analyze.py`
- Test: `scripts/tests/test_bench_scenario.py`, `scripts/tests/test_bench_analyze.py`, `scripts/tests/test_bench_run.py`

**Interfaces:**
- Produces: `Pose.sensor_id: int = 0` (0 EO, 1 IR, 2 NVG); `build_shots()` returns 15 shots: the 8 existing EO poses, EO `night_slant`, and `_ir`/`_nvg` variants of `nadir_3km`, `dusk_slant`, `night_slant` (the spec said 14; it left out the EO night shot that exit criterion 4 needs); `run_bench.py --sensor-path {auto,gpu,legacy}`; `run_bench.sensor_path_from_metrics(text) -> str | None`; `analyze.summarize` adds `sensor_gpu_ms_p50/p95` when rows have `sensor_gpu_ms >= 0`.

- [ ] **Step 1: Failing tests.**

`test_bench_scenario.py`:

```python
def test_shots_cover_ir_nvg_and_night():
    shots = {s.name: s for s in scenario.build_shots()}
    assert len(shots) == 15
    assert shots["night_slant"].pose.utc_hour == 4 and shots["night_slant"].pose.day == 22
    for base in ("nadir_3km", "dusk_slant", "night_slant"):
        assert shots[f"{base}_ir"].pose.sensor_id == 1
        assert shots[f"{base}_nvg"].pose.sensor_id == 2
    assert all(s.pose.sensor_id == 0 for n, s in shots.items() if not n.endswith(("_ir", "_nvg")))


def test_host_datagram_carries_the_sensor_id():
    pose = scenario.Pose(37.0, -122.0, 1000.0, sensor_id=2)
    dgram = scenario.host_datagram(1, pose)
    i = dgram.index(bytes([17, 24]))       # Sensor Control: packet id 17, size 24
    assert dgram[i + 4] == 2               # byte 4 = Sensor ID
```

`test_bench_analyze.py`:

```python
def test_sensor_gpu_ms_is_summarised_only_when_measured():
    rows = [{"t": 1.0 + i, "wall_ms": 33.3, "game_ms": 1, "render_ms": 1, "rhi_ms": 1, "gpu_ms": 10,
             "load_pct": 100.0, "families": 1, "dropped": 0, "emitted": i, "sensor_gpu_ms": v}
            for i, v in enumerate([1.0, 2.0, 3.0, -1.0])]
    out = analyze.summarize(rows, [{"name": "orbit", "start": 0.0, "end": 10.0, "measured": True}])
    assert out["orbit"]["sensor_gpu_ms_p50"] == 2.0
    legacy = [dict(r, sensor_gpu_ms=-1.0) for r in rows]
    assert "sensor_gpu_ms_p50" not in analyze.summarize(legacy, [{"name": "orbit", "start": 0.0, "end": 10.0, "measured": True}])["orbit"]
```

`test_bench_run.py`:

```python
def test_sensor_path_is_read_from_metrics():
    text = "# HELP x\ncamsim_uptime_seconds 3\ncamsim_sensor_path{path=\"gpu\"} 1\n"
    assert run_bench.sensor_path_from_metrics(text) == "gpu"
    assert run_bench.sensor_path_from_metrics("camsim_uptime_seconds 3\n") is None
```

Run: `python3 -m pytest scripts/tests -q` → the new tests fail.

- [ ] **Step 2: Implement.**

`scenario.py`: add `sensor_id: int = 0` to `Pose`; in `host_datagram` pass `sensor_id=pose.sensor_id` to `sc.build_host_frame`; in `build_shots` (non-smoke) add `night = replace(slant, utc_hour=4, utc_minute=40, day=22)  # ~21:40 PDT, sun ~10 deg below the horizon` and extend the list:

```python
    shots = [
        Shot("nadir_3km", nadir),
        Shot("slant_10km", slant),
        Shot("horizon", horizon),
        Shot("low_oblique", low_oblique),
        Shot("dawn_slant", replace(slant, utc_hour=13, utc_minute=15)),          # ~06:15 PDT
        Shot("dusk_slant", replace(slant, utc_hour=3, utc_minute=15, day=22)),   # ~20:15 PDT
        Shot("far_origin_slant", far_slant),
        Shot("far_origin_nadir", replace(nadir, lon=FAR_LON)),
        Shot("night_slant", night),
    ]
    by_name = {s.name: s.pose for s in shots}
    for base in ("nadir_3km", "dusk_slant", "night_slant"):
        shots.append(Shot(f"{base}_ir", replace(by_name[base], sensor_id=1)))
        shots.append(Shot(f"{base}_nvg", replace(by_name[base], sensor_id=2)))
    return shots
```

That is 9 + 6 = 15 shots; note the difference from the spec's 14 in the ROADMAP results.

`analyze.py` `summarize`: after building the dict, add

```python
        sensor = [r["sensor_gpu_ms"] for r in sel if r.get("sensor_gpu_ms", -1.0) >= 0.0]
        if sensor:
            out[ph["name"]]["sensor_gpu_ms_p50"] = _pct(sensor, 50)
            out[ph["name"]]["sensor_gpu_ms_p95"] = _pct(sensor, 95)
```

`run_bench.py`:

```python
def sensor_path_from_metrics(text: str) -> str | None:
    for line in text.splitlines():
        if line.startswith("camsim_sensor_path{"):
            return line.split('path="', 1)[1].split('"', 1)[0]
    return None
```

`--sensor-path` argument (`choices=["auto", "gpu", "legacy"]`, default `None`) sets `env["CAMSIM_RENDER_SENSOR_PATH"]`. After `wait_ready(pid_file)`: fetch `HEALTH + "/metrics"`; `path = sensor_path_from_metrics(body)`; print it; if `args.sensor_path in ("gpu", "legacy") and path != args.sensor_path: sys.exit(f"[bench] expected sensor path {args.sensor_path}, CamSim reports {path}")` (inside the `try` so `stop.sh` still runs). Record `"sensor_path": path` in `results["meta"]`. For each shot, fetch both images:

```python
            fetch_snapshot(out / "shots" / f"{shot.name}.png")
            fetch_snapshot(out / "shots" / f"{shot.name}_sensor.png", route="/snapshot/sensor")
```

and give `fetch_snapshot` a `route: str = "/snapshot"` parameter. IR/NVG shots need the sensor mode to arrive: keep the existing `time.sleep(1.0)` before the gate check.

- [ ] **Step 3: Run** `python3 -m pytest scripts/tests -q`. Expected: all pass.
- [ ] **Step 4: Update `scripts/bench/README.md`** — document `--sensor-path`, `_sensor.png` shots, the IR/NVG/night shots and `sensor_gpu_ms`.
- [ ] **Step 5: Commit** — `feat(bench): sensor path assertion, IR/NVG/night shots and sensor GPU timing (3B.1)`.

---

### Task 11: Calibrate, measure, document

**Files:**
- Modify: `deploy/camsim_config.yaml` (exposure defaults), `Source/CamSimTest/Camera/CamSimCaptureComponent.h` (`UeExposureOffsetEv`), `docs/configuration.md`, `ROADMAP.md`, `CLAUDE.md`
- Create: `scripts/bench/baselines/macos-m1pro-3b1-720p.json`, `…-1080p.json`, `scripts/bench/shots/macos/3b1/*.png`

- [ ] **Step 1: Calibrate the UE pre-exposure offset.** Temporarily log, once per second in `RunSensor_RenderThread`, `View.State ? View.State->GetPreExposure() : -1` next to `Params.Gain`, run the GPU path for a minute over the orbit (`--circle`), and set `UeExposureOffsetEv` so `log2(PreExposure) ≈ log2(Gain)` within ±2 EV in daylight. Remove the log. Record the value and why in the constant's comment.
- [ ] **Step 2: Calibrate exposure defaults.** Run `scripts/bench/run_bench.py --label 3b1-cal --sensor-path gpu --skip-warmup --smoke` and a full shot pass; read `scene_median_log2` from `frames.jsonl` for day shots and the night shot (and `_ir`/`_nvg`). Set per-mode `min_gain_ev`/`max_gain_ev` so: daylight medians expose to 0.18 inside the limits (not clamped); `night_slant` EO clamps at `max_gain_ev` with mean luma < 40; `night_slant_nvg` is not clamped (visible). Update `deploy/camsim_config.yaml`, the `FSensorExposureConfig` defaults if they should match, and `docs/configuration.md`; replace the "provisional" comments with the measured medians.
- [ ] **Step 3: Verify all tests.** NullRHI full suite (`CamSim`), `scripts/run_gpu_tests.sh`, `python3 -m pytest scripts/tests -q`, `node scripts/klv_conformance/check.js` if the conformance export test regenerated packets. Expected: all pass; paste the counts into the ROADMAP.
- [ ] **Step 4: Integration.** `CAMSIM_RENDER_SENSOR_PATH=gpu scripts/ci_validate.sh --native` → passes (video, 30 fps, KLV conformant). Then the same without the override (legacy path) → passes.
- [ ] **Step 5: Bench.** `scripts/bench/run_bench.py --label 3b1-720p --sensor-path gpu` and the same at 1080p (`CAMSIM_CAPTURE_WIDTH=1920 CAMSIM_CAPTURE_HEIGHT=1080`, real env names from `docs/configuration.md`), plus a `--sensor-path legacy` 720p run for comparison. Copy `results.json` files to `scripts/bench/baselines/macos-m1pro-3b1-{720p,1080p}.json` and the 720p GPU run's `*_sensor.png` into `scripts/bench/shots/macos/3b1/`. Check against the 3B.1 part of the exit criteria: `sensor_path=gpu` all run; 30 fps / 0 dropped per phase; frame p95 ≤ 3A.1 baseline + 1 ms; `sensor_gpu_ms` p95 recorded (≤ 4 ms at 1080p is the full-3B target — record the 3B.1 number, it has fewer effects); daylight EO mean luma 90–170 with < 1% clipped and `night_slant` EO mean < 40 (compute with `python3 -c` + `numpy`/`PIL` if installed, else `ffmpeg -i shot.png -vf signalstats -f null -` and read `YAVG`).
- [ ] **Step 6: Documentation.**
  - `ROADMAP.md` Milestone 3: a "3B.1 status" paragraph with the measured table (legacy vs GPU: frame p95, GPU p50, sensor GPU ms, emitted fps, drops), exposure results (day/night luma), the 9 refinements from this plan's header, and the next step (3B.2 plan). Fix item 5's stale text ("today only one readback is in flight" → the 3A.1 ring).
  - `CLAUDE.md` Gotchas: replace the Readback-ring bullet's last sentence ("if the sensor model can't keep up … until 3B moves it to the GPU") with the GPU-path note; add bullets: "**Sensor path** (`render.sensor_path`, ROADMAP 3B): `gpu` replaces UE's tonemapper via `ISceneViewExtension::EPostProcessingPass::ReplacingTonemapper`; UE exposure is manual and driven by the sensor AE; `auto` falls back to legacy while any unported effect is enabled", "**Shaders** live in `unreal_project/CamSimTest/Shaders/` (virtual path `/CamSim`), compiled by the `CamSimShaders` module (`PostConfigInit`)"; update the Architecture tree (new module, `Shaders/`) and the test count.
  - `docs/configuration.md`: final defaults.
- [ ] **Step 7: Visual review request.** Tell the user the shot set is in `scripts/bench/shots/macos/3b1/` (EO day/dusk/night, IR, NVG) and ask for review; 3B.1 is not "done" in the ROADMAP until they sign off.
- [ ] **Step 8: Commit** — `docs: ROADMAP 3B.1 results (GPU sensor pipeline on macOS)`.
