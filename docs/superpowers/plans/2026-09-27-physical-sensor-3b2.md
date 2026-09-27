# Physical Sensor Model 3B.2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the legacy CPU sensor model with a physically based GPU sensor model (optics → electrons → detector noise → ADC → display) for EO (HD CMOS) and IR (cooled MWIR default, uncooled LWIR optional), and make it the only sensor path.

**Architecture:** First delete everything only the legacy path needed (NVG, CPU model, overlays, scene-capture view source, BGRA encode, legacy effect keys) so the 3B.1 GPU graph is unconditional. Then add preset-driven optics/detector config, split the AE gain into photon and analog gain in the controller, build the model stage by stage in the CPU reference (with physics tests that prove the statistics), mirror it in RDG compute passes (Optics → BlurH → BlurV → Detector → PackNv12), wire the parameters, and calibrate on the bench.

**Tech Stack:** UE 5.8 C++ (RDG, global compute shaders, HLSL), FFmpeg, UE Automation tests (NullRHI + Metal GPU tests), Python bench.

**Spec:** `docs/superpowers/specs/2026-09-27-physical-sensor-model-design.md` (revises 3B.2–3B.4 of `2026-09-27-gpu-sensor-model-design.md`).

## Global Constraints

- macOS/Metal verified; nothing Metal-specific: plain compute, **integer atomics only, no float atomics, no wave intrinsics**.
- Copyright header on new C++/HLSL files: `// Copyright CamSim Contributors. All Rights Reserved.`; UE naming; `CoreMinimal.h` first; forward declarations in headers.
- New config keys documented in `docs/configuration.md` and `deploy/camsim_config.yaml`; removed keys disappear from both (the loader's unknown-key warning handles leftovers).
- Sensor state uses `FSimClock` sim time, never `DeltaTime`.
- NV12: width % 4 == 0, even height; BT.709 limited range. Histogram: 256 bins, log2 [-16, +16), 8 bins/stop, measured on the **noiseless** signal.
- Randomness: PCG hash of (x, y, frame index, seed, stream id); temporal fields include the frame index, fixed-pattern fields do not; Gaussian via Box-Muller from two hashed uniforms in (0,1).
- Rounding: `floor(x + 0.5)` everywhere (GPU and reference), never HLSL `round`.
- GPU vs reference: Y ≤ 1 DN, UV ≤ 2 DN; histogram bin-exact.
- Sensor graph GPU p95 ≤ 2 ms at 1080p (M1 Pro).
- Commits in repo style ending with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
  ```

### Commands

```bash
set -o pipefail; scripts/run.sh --build-only 2>&1 | tail -5                    # build
# NullRHI tests (filter as needed)
UE_BIN="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
rm -rf .cache/automation-report
"$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" -ExecCmds="Automation RunTests CamSim+Quit" \
  -TestExit="Automation Test Queue Empty" -ReportExportPath="$PWD/.cache/automation-report" \
  -unattended -nullrhi -nosound -nosplash -DisablePython -log -stdout -FullStdOutLogOutput > .cache/automation.log 2>&1
python3 -c "import json;d=json.load(open('.cache/automation-report/index.json',encoding='utf-8-sig'));print('succeeded',d['succeeded'],'failed',d['failed']);[print('FAIL',t['fullTestPath'],[e['event']['message'] for e in t['entries'] if e['event']['type']=='Error']) for t in d['tests'] if t['state']=='Fail']"
scripts/run_gpu_tests.sh                                                          # Metal GPU tests
uv run --with pytest --with numpy --with pillow python -m pytest scripts/tests -q  # bench tests
```

Baseline before 3B.2: NullRHI 278 pass (+2 expected warnings), GPU 5/5, pytest 42.

## Review Focus

1. **Extreme lighting at the detector boundaries** (direct sun glint → full well; moonless night → a handful of electrons): output must saturate/clip cleanly and never produce NaN or negative electrons (shot noise of a negative value). → Task 7 test `ClipsAtFullWellAndZero`.
2. **The IR AGC band collapsing** (a uniform thermal scene, all pixels in one histogram bin) after the photon-gain scaling: the display gain must stay finite. → Task 6 test `IrAgcBandNeverZeroAfterScaling`.
3. **Distortion at the frame corners with strong barrel k1** (e.g. k1 = −0.3 at 60° HFOV): Newton must converge or the config must be rejected at load, never a black-hole artefact mid-frame. → Task 8 test `NewtonConvergesOrConfigRejected` + Task 10 validation.
4. **Very narrow zoom FOV (1°) and wide FOV (60°)**: focal length in pixels, vignetting and distortion stay finite and sensible; PSF in pixels does not depend on FOV. → Task 8 test `OpticsAcrossZoomRange`.
5. **Frame index wrap / seed 0**: the hash must not degenerate (seed 0 and frame 0 still produce Gaussian fields with mean ≈ 0, σ ≈ 1). → Task 7 test `HashGaussianStatistics`.

---

## File map

| File | Change |
| --- | --- |
| `Source/CamSimTest/Sensor/SensorPostProcess.{h,cpp}`, `Sensor/IPixelPipeline.h`, `Overlay/*`, `Sensor/SensorPath.{h,cpp}` | delete |
| `Source/CamSimTest/Tests/SensorPostProcessTest.cpp`, `SensorFidelityTest.cpp`, `OverlayTest.cpp`, `OpticalRealismTest.cpp`, `Phase18Test.cpp` (overlay parts), `Phase21StreamingTest.cpp` (laser-spot parts), `SensorPathTest.cpp` (selector parts) | delete / trim |
| `Source/CamSimTest/Sensor/SensorTypes.h`, `Config/CamSimConfig.{h,cpp}` | NVG + legacy keys out; presets, optics, detector in |
| `Source/CamSimTest/Sensor/SensorPresets.{h,cpp}` | new: preset tables + resolution |
| `Source/CamSimShaders/Public/SensorFrameParams.h` | params grow: optics, detector, ADC, frame index, seed |
| `Source/CamSimShaders/Public/SensorHash.h` | new: PCG hash + Gaussian (C++; mirrored in HLSL) |
| `Source/CamSimTest/Sensor/SensorController.{h,cpp}` | exposure split, IR AGC in normalised DN |
| `Source/CamSimTest/Sensor/SensorReference.{h,cpp}` | stage functions: Optics, Blur, Detector, Display, PackNv12 |
| `Source/CamSimTest/Sensor/SensorOptics.{h,cpp}` | new: focal px, distortion inverse, PSF σ/weights (shared by reference + wiring) |
| `unreal_project/CamSimTest/Shaders/Private/CamSimSensor.usf`, `CamSimSensorCommon.ush` (new) | new passes |
| `Source/CamSimShaders/Private/SensorGraph.cpp`, `Public/SensorGraph.h` | pass list |
| `Source/CamSimTest/Camera/CamSimCaptureComponent.{h,cpp}`, `CamSimFrameGrabExtension.{h,cpp}`, `CamSimCamera.{h,cpp}` | legacy branches out, params wiring |
| `Source/CamSimTest/Encoder/*` | NV12 only |
| `Source/CamSimTest/Tests/SensorPhysicsTest.cpp` (new), `SensorGpuTest.cpp`, `SensorControllerTest.cpp`, `SensorConfigTest.cpp`, `SensorReferenceTest.cpp` | tests |
| `deploy/camsim_config.yaml`, `docs/configuration.md`, `ROADMAP.md`, `CLAUDE.md`, `scripts/bench/*` | docs, bench |

(`Source/…` = `unreal_project/CamSimTest/Source/…`.)

---

### Task 1: Remove NVG

**Files:** `Sensor/SensorTypes.h` (`ESensorMode`), `Camera/CamSimSensorComponent.cpp:38`, `Hosts/CigiCommands.cpp`, `Metadata/KlvBuilder.cpp:111`, `Config/CamSimConfig.cpp` (built-in NVG defaults, `ParseMode("nvg", …)`), `CamSimShaders/Public/SensorFrameParams.h` (`ESensorGraphMode::NVG`), `Sensor/SensorController.cpp` (NVG tint/branch), shader `Mode == 2` paths, `Sensor/SensorReference.cpp`, tests referencing NVG, `deploy/camsim_config.yaml` (`sensor_modes.nvg`), `docs/configuration.md`, `scripts/bench/scenario.py` (`_nvg` shots).

**Interfaces:** Produces `enum class ESensorMode : uint8 { EO = 0, IR = 1 }` and `enum class ESensorGraphMode : uint32 { EO = 0, IR = 1 }`; `FSensorFrameParams::DisplayTint` removed.

- [ ] **Step 1: Failing tests.** In `Tests/SensorConfigTest.cpp` add:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorNoNvgTest, "CamSim.Sensor.Config.NvgRemoved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorNoNvgTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  nvg:\n    target_grey: 0.3\n"));
	TestEqual(TEXT("only EO and IR modes"), Cfg.SensorModeConfigs.Num(), 2);
	TestTrue(TEXT("nvg block reported as unknown"), Cfg.UnknownYamlKeys.ContainsByPredicate(
		[](const FString& K) { return K.Contains(TEXT("nvg")); }));
	return true;
}
```

and in the CIGI test file that covers Sensor Control (`grep -rln "SensorId" Tests`), a test `CamSim.Cigi.SensorIdTwoMapsToEo` asserting Sensor ID 2 selects `ESensorMode::EO` (drive `UCamSimSensorComponent` the way the existing sensor-control test does). In `Tests/KlvBuilderTest.cpp` (or the file asserting Tag 11), assert SensorMode 1 → "IR" and 2 → "EO".
- [ ] **Step 2: Build/run** — fails.
- [ ] **Step 3: Remove NVG.** Enum loses `NVG`; `CamSimSensorComponent` maps `SensorId` 0 → EO, 1 → IR, anything else → EO with `UE_LOG(... Warning ...)` once per session (static bool). KLV Tag 11: `(T.SensorMode == 1) ? "IR" : "EO"`. Delete the built-in NVG `FSensorModeConfig` and `ParseMode("nvg", …)`. Delete every `NVG`/`Mode == 2`/`DisplayTint` branch in controller, reference, shader (IR keeps `.a = v`, `.rgb = v`). Remove NVG shots from `scripts/bench/scenario.py` and its test (15 → 12 shots: 9 EO + IR at nadir/dusk/night). Remove `sensor_modes.nvg` from the yaml and docs.
- [ ] **Step 4:** `git grep -n "NVG\|Nvg\|nvg" -- unreal_project/CamSimTest/Source deploy docs/configuration.md scripts/bench` returns only intentional mentions (the warning text, docs sentence "NVG was removed"). Build; full NullRHI suite; GPU tests; pytest — all pass (NVG GPU test `NvgNight` deleted).
- [ ] **Step 5: Commit** — `refactor(sensor): remove NVG (EO and IR only)`.

---

### Task 2: Remove the legacy sensor path and overlays

**Files (delete):** `Sensor/SensorPostProcess.{h,cpp}`, `Sensor/IPixelPipeline.h`, `Overlay/` (whole directory), `Sensor/SensorPath.{h,cpp}`, `Tests/SensorPostProcessTest.cpp`, `Tests/SensorFidelityTest.cpp`, `Tests/OverlayTest.cpp`, `Tests/OpticalRealismTest.cpp` (distortion tests of the CPU model).
**Files (modify):** `Camera/CamSimCaptureComponent.{h,cpp}`, `Camera/CamSimFrameGrabExtension.{h,cpp}`, `Camera/CamSimCamera.{h,cpp}`, `Subsystem/CamSimSubsystem.{h,cpp}`, `Config/CamSimConfig.{h,cpp}` (`render.sensor_path`, `performance.gpu_sensor_*`, `overlay`, `laser_designator` drawing fields, `phase18.precipitation` overlay), `Scenario/ScenarioRandomizer.cpp:60`, `Tests/SensorPathTest.cpp`, `Tests/Phase18Test.cpp`, `Tests/Phase21StreamingTest.cpp`, `Tests/Phase27PerformanceTest.cpp`, `Tests/RenderConfigTest.cpp`, `Tests/SensorConfigTest.cpp`, `Encoder/VideoEncoder.cpp` (transfer tag decision), yaml, docs.

**Interfaces:**
- Produces `UCamSimSubsystem::IsSensorGraphAvailable() const` (true when `IsSensorGraphSupported` passed at `Initialize`), replacing `GetSensorPathDecision()`. `/metrics` keeps `camsim_sensor_path{path="gpu"} 1` (bench compatibility) when available.
- `FVideoEncoder` constructor no longer takes a sensor path (always BT.709 transfer).

- [ ] **Step 1: Failing test.** `Tests/SensorPathTest.cpp` keeps `ShaderModule.LoadedWithShaderDirectory` and `Histogram.BinOf`; replace the selector tests with:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorLegacyKeysGoneTest, "CamSim.Sensor.Config.LegacyPathKeysUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorLegacyKeysGoneTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"render:\n  sensor_path: legacy\n"
		"overlay:\n  enabled: true\n"
		"performance:\n  gpu_sensor_effects: true\n"));
	for (const TCHAR* Key : { TEXT("sensor_path"), TEXT("overlay"), TEXT("gpu_sensor_effects") })
	{
		TestTrue(FString::Printf(TEXT("%s reported unknown"), Key),
			Cfg.UnknownYamlKeys.ContainsByPredicate([Key](const FString& K) { return K.Contains(Key); }));
	}
	return true;
}
```

- [ ] **Step 2: Build** — the test fails (keys still parsed).
- [ ] **Step 3: Delete and simplify.**
  - Capture component: `bGpuSensor` is gone — the GPU sensor graph always runs in the primary view. Delete `SensorFX`, `SensorPath`, the BGRA `RenderTargets` ring and `ColorReadbackPool` (keep `DepthRenderTargets`/`DepthReadbackPool` for ML depth), the texture branch in `EnqueuePoll`, `S.Pixels`, BGRA `OfferSnapshot` branch, the CPU sensor task in `SubmitFrameToEncoder` (keep the ground-truth collector work on the background task), `ApplyRenderSettings`' legacy auto-exposure `else` branch (manual exposure always).
  - Frame-grab extension: delete `PostRenderViewFamily_RenderThread`'s backbuffer grab and the texture targets in `FTargets`/`PushRequest_RenderThread` (NV12 readback only); `SubscribeToPostProcessingPass` no longer checks a flag.
  - Subsystem: `Initialize` calls `IsSensorGraphSupported(Why)`; on failure log `Error` "sensor graph unavailable: <Why>" and keep `/ready` false (make the readiness lambda return false while `!IsSensorGraphAvailable()`); the encoder always tags BT.709.
  - Camera: delete `UpdateLaserDesignator()` and its call; delete anything that fed the HUD.
  - Config: delete `FRenderConfig::SensorPath/SensorPathMode/ESensorPath` + parsing + env `CAMSIM_RENDER_SENSOR_PATH`; `Performance.bGpuSensorEffects`, `GpuSensorMaterialPath`, `GpuSensorMpcPath` + env; the `overlay` section (`FHudOverlayConfig`); `LaserDesignator` spot fields (keep whatever DIS/KLV telemetry uses — `git grep -n "LaserDesignator" -- Source` after deleting the camera's projection and remove the config only if nothing else reads it); `Phase18.bPrecipitation` (and `ScenarioRandomizer.cpp:60`, which set it). Validation (`Validate()`) loses the `WantsGpu` gating: the NV12 width check is unconditional again (the GPU path is the only path).
  - Tests: delete the listed files; trim the listed ones to what still exists.
  - Docs/yaml: remove the keys; `docs/configuration.md` `render.sensor_path` row → removed; add a short "Removed in 3B.2" list.
- [ ] **Step 4:** `git grep -n "SensorPostProcess\|IPixelPipeline\|FHudOverlay\|SensorPathMode\|FSensorPathSelector\|bGpuSensorEffects\|UpdateLaserDesignator\|bPrecipitation" -- unreal_project/CamSimTest/Source` returns nothing. Build; NullRHI suite; GPU tests; live smoke `python3 scripts/bench/run_bench.py --smoke --label t2-smoke` passes (drop `--sensor-path` from `run_bench.py` here: the flag and its assertion go; keep reading `camsim_sensor_path` into meta).
- [ ] **Step 5: Commit** — `refactor(sensor): remove the legacy CPU sensor path, overlays and path selector`.

---

### Task 3: Remove the scene-capture view source and BGRA encoding

**Files:** `Config/CamSimConfig.{h,cpp}` (`FRenderConfig::EViewSource`, `ViewSource`, `IsPrimary()` callers), `Camera/CamSimCaptureComponent.cpp` (`CaptureScene` branch, FXAA branch), `Camera/CamSimCamera.cpp` (`ApplyPrimaryView` conditionals), `Camera/CamSimStreamingController.cpp` (`NumStreamingCameras`), `Geospatial/CesiumTuning.cpp` (`UseLodTransitions`), `Encoder/IFrameSink.h`, `Encoder/VideoEncoder.{h,cpp}`, `Encoder/MultiViewFrameSink.{h,cpp}`, `Encoder/EncoderThread.*`, tests (`RenderConfigTest.cpp`, `RenderPathTest.cpp`, `EncoderThreadTest.cpp`, `VideoEncoderRateControlTest.cpp`, `EncoderNv12Test.cpp`), `scripts/bench/run_bench.py` (`--view-source`), yaml, docs.

**Interfaces:** `FSensorFrame` becomes `{ TArray<uint8> Nv12; }` (no format enum); `IFrameSink::EncodeFrame(const FSensorFrame&, …)` unchanged in signature.

- [ ] **Step 1: Failing test.** In `Tests/RenderConfigTest.cpp` replace the view-source tests with `CamSim.Config.ViewSourceKeyRemoved` (loading `render:\n  view_source: scene_capture\n` reports `view_source` as unknown). In `Tests/VideoEncoderRateControlTest.cpp` convert the BGRA frame generator to NV12 (Y = the grey value mapped to limited range `16 + V*219/255`, UV = 128) so the rate-control assertions still hold.
- [ ] **Step 2: Build** — fails.
- [ ] **Step 3: Implement.** Delete `EViewSource`/`ViewSource`/`ParseViewSource`/env `CAMSIM_RENDER_VIEW_SOURCE`; every `IsPrimary()` branch keeps its primary-view side. The `SceneCapture` component stays as the pose/FOV/post-process holder (it is never captured; the ML depth capture is a separate component). Encoder: delete the BGRA/grayscale/sws code, `SwsCtx`, `RgbCompressedScratch`, `bSwsColorSpaceApplied`, `ESensorPixelFormat`; MultiView: delete `ApplyDigitalZoom` (BGRA) and keep `ApplyDigitalZoomNv12`. `UseLodTransitions(Cfg)` returns `Cfg.bUseLodTransitions`. Bench: drop `--view-source`.
- [ ] **Step 4:** `git grep -n "EViewSource\|SceneCapture)\|ESensorPixelFormat\|sws_scale\|ApplyDigitalZoom(" -- unreal_project/CamSimTest/Source` returns only `ApplyDigitalZoomNv12`. Build; NullRHI; GPU; pytest; `scripts/ci_validate.sh --native` passes.
- [ ] **Step 5: Commit** — `refactor(render): primary view only; encoder takes NV12 only`.

---

### Task 4: Remove legacy effect config keys

**Files:** `Sensor/SensorTypes.h`, `Config/CamSimConfig.{h,cpp}` (ParseMode fields, `sensor_quality`, `sensor_quality_profiles`, `ActiveSensorQuality`, `OpticalRealism.bLensDistortion/DistortionK1/K2/bChromaticAberration/ChromaticAberrationIntensity`, `Phase18.bDynamicIRExtinction`), `Camera/CamSimCaptureComponent.cpp` (chromatic aberration block), tests (`CamSimConfigTest.cpp`, `ConfigLoadTest.cpp`, `SensorConfigTest.cpp`, `Phase27PerformanceTest.cpp`, `Phase18Test.cpp`), yaml, docs.

**Interfaces:** `FSensorModeConfig` becomes:

```cpp
struct FSensorModeConfig
{
	FVector3f SignalWeights = FVector3f(0.2126f, 0.7152f, 0.0722f);
	FSensorExposureConfig Exposure;
	bool  bAGCEnabled       = false;   // IR percentile stretch
	float AGCLowPercentile  = 0.01f;
	float AGCHighPercentile = 0.99f;
	int32 AGCLagFrames      = 0;
	// optics/detector/preset added in Task 5
};
```

- [ ] **Step 1: Failing test.** `Tests/SensorConfigTest.cpp`: `CamSim.Sensor.Config.LegacyEffectKeysUnknown` loads a yaml with `sensor_modes.eo.noise_netd`, `sensor_modes.ir.column_banding`… use the real old names: `noise_netd`, `fixed_pattern_noise`, `vignetting`, `scan_lines`, `gaussian_sigma`, `defect_pixel_count`, `quantization_bits`, `ac_banding_amplitude`, `thermal_drift_enabled`, `sun_glint_intensity`, `contrast`, plus top-level `sensor_quality`, `optical_realism.lens_distortion`, `optical_realism.chromatic_aberration` — each must appear in `UnknownYamlKeys`. Also `CamSim.Config.CanonicalConfigHasNoUnknownKeys` must still pass after the yaml is cleaned.
- [ ] **Step 2: Build** — fails.
- [ ] **Step 3: Delete** the fields, parsing, env overrides, presets/profiles, validation of removed fields, the chromatic-aberration post-process block, and their yaml/doc entries. Keep `optical_realism` for UE effects (motion blur, bloom, DoF, lens flare).
- [ ] **Step 4:** build; NullRHI; GPU; pytest.
- [ ] **Step 5: Commit** — `refactor(config): remove legacy sensor effect keys and quality presets`.

---

### Task 5: Presets, optics and detector config

**Files:** create `Sensor/SensorPresets.{h,cpp}`; modify `Sensor/SensorTypes.h`, `Config/CamSimConfig.{h,cpp}`; test `Tests/SensorConfigTest.cpp`; yaml; docs.

**Interfaces (produces, `SensorTypes.h`):**

```cpp
enum class ESensorDetectorType : uint8 { Photon = 0, Microbolometer = 1 };

struct FSensorOpticsConfig
{
	float FNumber            = 4.0f;
	float PixelPitchUm       = 2.9f;
	float WavelengthUm       = 0.55f;
	float ExtraBlurPx        = 0.0f;
	float VignettingExponent = 4.0f;
	float K1 = 0.0f, K2 = 0.0f;
};

struct FSensorDetectorConfig
{
	ESensorDetectorType Type = ESensorDetectorType::Photon;
	float FullWellE      = 10000.0f;   // photon
	float ReadNoiseE     = 2.0f;
	float Prnu           = 0.01f;
	float DsnuE          = 1.0f;
	float DarkCurrentEs  = 5.0f;
	float MaxAnalogGainDb = 30.0f;
	float TemporalNoise  = 0.0f;       // microbolometer, fractions of full scale
	float PixelFpn = 0.0f, ColumnFpn = 0.0f, RowFpn = 0.0f;
	int32 AdcBits        = 12;
	float HotPixelFraction  = 1e-5f;
	float DeadPixelFraction = 1e-5f;
};
// FSensorModeConfig gains: FString Preset; uint32 Seed = 1; FSensorOpticsConfig Optics; FSensorDetectorConfig Detector;
// FSensorExposureConfig: rename MaxGainEv -> MaxPhotonGainEv (yaml max_photon_gain_ev).
```

`SensorPresets.h`: `bool CamSimSensorPresets::Apply(const FString& Name, FSensorModeConfig& InOut);` (false for unknown names) with the three presets from the spec table exactly: `eo_hd_cmos`, `mwir_cooled`, `lwir_uncooled`.

- [ ] **Step 1: Failing tests** (`SensorConfigTest.cpp`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPresetDefaultsTest, "CamSim.Sensor.Config.PresetDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPresetDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(""));
	const FSensorModeConfig& Eo = Cfg.SensorModeConfigs.FindChecked(ESensorMode::EO);
	const FSensorModeConfig& Ir = Cfg.SensorModeConfigs.FindChecked(ESensorMode::IR);
	TestEqual(TEXT("eo preset"), Eo.Preset, FString(TEXT("eo_hd_cmos")));
	TestEqual(TEXT("eo full well"), Eo.Detector.FullWellE, 10000.0f);
	TestEqual(TEXT("eo pitch"), Eo.Optics.PixelPitchUm, 2.9f);
	TestEqual(TEXT("eo adc"), Eo.Detector.AdcBits, 12);
	TestEqual(TEXT("ir preset"), Ir.Preset, FString(TEXT("mwir_cooled")));
	TestTrue(TEXT("ir photon"), Ir.Detector.Type == ESensorDetectorType::Photon);
	TestEqual(TEXT("ir full well"), Ir.Detector.FullWellE, 7000000.0f);
	TestEqual(TEXT("ir read noise"), Ir.Detector.ReadNoiseE, 400.0f);
	TestEqual(TEXT("ir wavelength"), Ir.Optics.WavelengthUm, 4.0f);
	TestEqual(TEXT("ir adc"), Ir.Detector.AdcBits, 14);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPresetOverrideTest, "CamSim.Sensor.Config.PresetOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPresetOverrideTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n  ir:\n    preset: lwir_uncooled\n    seed: 7\n"
		"    detector:\n      column_fpn: 0.002\n    optics:\n      k1: -0.05\n"));
	const FSensorModeConfig& Ir = Cfg.SensorModeConfigs.FindChecked(ESensorMode::IR);
	TestTrue(TEXT("microbolometer"), Ir.Detector.Type == ESensorDetectorType::Microbolometer);
	TestEqual(TEXT("preset value kept"), Ir.Detector.TemporalNoise, 0.004f);
	TestEqual(TEXT("override wins"), Ir.Detector.ColumnFpn, 0.002f);
	TestEqual(TEXT("optics override"), Ir.Optics.K1, -0.05f);
	TestEqual(TEXT("seed"), Ir.Seed, 7u);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPresetValidationTest, "CamSim.Sensor.Config.PresetValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPresetValidationTest::RunTest(const FString& Parameters)
{
	auto Errors = [](const TCHAR* Yaml) { return FCamSimConfig::LoadFromYamlString(Yaml).Validate(); };
	auto Has = [](const TArray<FString>& E, const TCHAR* S) { return E.ContainsByPredicate([S](const FString& X) { return X.Contains(S); }); };
	TestTrue(TEXT("unknown preset"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    preset: hd55\n")), TEXT("preset")));
	TestTrue(TEXT("full well"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      full_well_e: 0\n")), TEXT("full_well_e")));
	TestTrue(TEXT("adc bits"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      adc_bits: 20\n")), TEXT("adc_bits")));
	TestTrue(TEXT("negative noise"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      read_noise_e: -1\n")), TEXT("read_noise_e")));
	TestTrue(TEXT("defect fraction"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    detector:\n      hot_pixel_fraction: 0.5\n")), TEXT("hot_pixel_fraction")));
	TestTrue(TEXT("k1 range"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      k1: 2\n")), TEXT("k1")));
	TestTrue(TEXT("f-number"), Has(Errors(TEXT("sensor_modes:\n  eo:\n    optics:\n      f_number: 0\n")), TEXT("f_number")));
	return true;
}
```

- [ ] **Step 2: Build** — fails.
- [ ] **Step 3: Implement.** Parse order per mode: read `preset` (default `eo_hd_cmos` for EO, `mwir_cooled` for IR), `CamSimSensorPresets::Apply` (unknown name: keep defaults, record for `Validate()`), then read overrides from `optics:`/`detector:` blocks (keys: `f_number`, `pixel_pitch_um`, `wavelength_um`, `extra_blur_px`, `vignetting_exponent`, `k1`, `k2`; `type` (`photon`|`microbolometer`), `full_well_e`, `read_noise_e`, `prnu`, `dsnu_e`, `dark_current_e_s`, `max_analog_gain_db`, `temporal_noise`, `pixel_fpn`, `column_fpn`, `row_fpn`, `adc_bits`, `hot_pixel_fraction`, `dead_pixel_fraction`) and `seed`. Exposure: `max_photon_gain_ev` replaces `max_gain_ev` (EO keeps its calibrated −12.5 as the photon limit; IR −6). Validation per the spec's list. Update the calibrated-exposure tests (`PerModeExposureDefaults`/`PerModeExposureCanonicalConfig`) for the rename. Yaml: each mode gets `preset`, `seed`, and commented `optics:`/`detector:` blocks listing the preset values; docs table of presets and keys.
- [ ] **Step 4:** tests pass; full NullRHI.
- [ ] **Step 5: Commit** — `feat(sensor): sensor-class presets with optics and detector config (3B.2)`.

---

### Task 6: Parameters and the exposure split

**Files:** `CamSimShaders/Public/SensorFrameParams.h`, `Sensor/SensorController.{h,cpp}`, `Tests/SensorControllerTest.cpp`.

**Interfaces:**

```cpp
struct FSensorFrameParams
{
	ESensorGraphMode Mode = ESensorGraphMode::EO;
	uint32    bBlackHot = 0;
	FVector3f SignalWeights = FVector3f(0.2126f, 0.7152f, 0.0722f);
	float     InputScale = 1.0f;
	uint32    Serial = 0;
	// exposure
	float PhotonGain = 1.0f;       // signal -> fraction of full scale before noise
	float AnalogGain = 1.0f;       // applied after detector noise (photon detectors)
	float DisplayGain = 1.0f;      // IR AGC on normalised DN; EO 1
	float DisplayOffset = 0.0f;
	float KneeStart = 0.8f;
	// detector (copied from FSensorDetectorConfig; DarkE = DarkCurrentEs / frame rate)
	uint32 DetectorType = 0; float FullWellE = 10000.0f, ReadNoiseE = 2.0f, Prnu = 0.01f, DsnuE = 1.0f, DarkE = 0.0f;
	float TemporalNoise = 0.0f, PixelFpn = 0.0f, ColumnFpn = 0.0f, RowFpn = 0.0f;
	float AdcMax = 4095.0f;        // 2^bits - 1
	float HotFraction = 1e-5f, DeadFraction = 1e-5f;
	uint32 Seed = 1, FrameIndex = 0;
	// optics (Task 8 fills these)
	float FocalPx = 0.0f;          // 0 = optics off (tests)
	float K1 = 0.0f, K2 = 0.0f, VignettingExponent = 0.0f;
	float PsfSigmaPx = 0.0f;
};
```

Old `Gain`/`Offset` are removed; 3B.1 tests move to `PhotonGain`/`DisplayGain`/`DisplayOffset`.

Controller: the 3B.1 AE computes a total gain EV `G` exactly as now (target grey, highlight cap, lag, snaps, clamped to `[MinGainEv, MaxPhotonGainEv + MaxAnalogGainDb/20·log2(10)]`). Split:
`PhotonEv = min(G, MaxPhotonGainEv)`, `AnalogEv = G − PhotonEv` (≥ 0);
`PhotonGain = 2^PhotonEv`, `AnalogGain = 2^AnalogEv`. Microbolometer and IR (no analog stage): `AnalogGain = 1`, cap is `MaxPhotonGainEv`.
IR AGC (enabled): percentile band `[Lo, Hi]` in signal units from the noiseless histogram; photon gain from the AE computation as for EO; `lo_n = Lo·PhotonGain`, `hi_n = max(Hi·PhotonGain, lo_n + 1e-6)`; `DisplayGain = 1/(hi_n − lo_n)`, `DisplayOffset = −lo_n·DisplayGain`.
`FrameIndex = In.Serial`; `Seed`, detector fields from config; `DarkE = DarkCurrentEs / 30`.

- [ ] **Step 1: Failing tests** (`SensorControllerTest.cpp`): update existing tests to the new fields (their numeric expectations are unchanged for the photon-limited range, `AnalogGain == 1`), and add:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorExposureSplitTest, "CamSim.Sensor.Controller.PhotonGainBeforeAnalogGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorExposureSplitTest::RunTest(const FString& Parameters)
{
	FSensorModeConfig Cfg; Cfg.Exposure.MinGainEv = -20; Cfg.Exposure.MaxPhotonGainEv = -12; Cfg.Exposure.LagFrames = 0;
	Cfg.Detector.MaxAnalogGainDb = 24.0f;                   // 24 dB = 3.99 EV
	FSensorController C;
	const FSensorHistogram Day = Flat(12.0f, 1);            // needs ~-14.5 EV: photon only
	FSensorFrameParams P = C.Update(In(&Day, 1), Cfg);
	TestEqual(TEXT("day: no analog gain"), P.AnalogGain, 1.0f);
	const FSensorHistogram Dusk = Flat(8.0f, 2);            // needs ~-10.5: photon capped at -12, +1.5 EV analog
	FSensorControllerInput I = In(&Dusk, 2); I.bCameraCut = true;
	C.Update(I, Cfg); P = C.Update(In(&Dusk, 3), Cfg);
	TestEqual(TEXT("dusk: photon gain capped"), FMath::Log2(P.PhotonGain), -12.0f, 1e-3f);
	TestTrue(TEXT("dusk: analog gain used"), P.AnalogGain > 1.5f);
	const FSensorHistogram Night = Flat(0.0f, 4);           // needs ~-2.5: capped at -12 + 3.99
	I = In(&Night, 4); I.bCameraCut = true; C.Update(I, Cfg); P = C.Update(In(&Night, 5), Cfg);
	TestEqual(TEXT("night: analog capped"), FMath::Log2(P.AnalogGain), 24.0f / 20.0f * FMath::Log2(10.0f), 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorIrAgcScaledTest, "CamSim.Sensor.Controller.IrAgcBandNeverZeroAfterScaling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorIrAgcScaledTest::RunTest(const FString& Parameters)
{
	FSensorModeConfig Cfg; Cfg.bAGCEnabled = true; Cfg.AGCLagFrames = 0; Cfg.Exposure.LagFrames = 0;
	FSensorController C;
	FSensorHistogram H; H.Bins[FSensorHistogram::BinOf(8.0f)] = 1000; H.Serial = 1;   // one bin
	FSensorControllerInput I = In(&H, 1); I.Mode = ESensorGraphMode::IR;
	const FSensorFrameParams P = C.Update(I, Cfg);
	TestTrue(TEXT("finite display gain"), FMath::IsFinite(P.DisplayGain) && P.DisplayGain > 0.0f);
	TestTrue(TEXT("finite offset"), FMath::IsFinite(P.DisplayOffset));
	return true;
}
```

(`Flat`/`In` are the existing helpers in that file.)
- [ ] **Step 2–4:** fail, implement, pass. Also: `CamSimCaptureComponent::UpdateSensorParams` fills `Seed` and the detector fields (optics stay 0 until Task 10); rename the shader/reference fields (`Gain` → `PhotonGain`, `Offset` → `DisplayOffset`, IR adds `DisplayGain`). Until the detector lands (Tasks 7 and 9) the display path keeps 3B.1's behaviour on `signal × PhotonGain × AnalogGain`, so the 3B.1 GPU/reference tests pass with only the field renames.
- [ ] **Step 5: Commit** — `feat(sensor): split AE gain into photon and analog gain`.

---

### Task 7: Reference detector, ADC and hash (physics tests)

**Files:** create `CamSimShaders/Public/SensorHash.h`, `Tests/SensorPhysicsTest.cpp`; modify `Sensor/SensorReference.{h,cpp}`.

**Interfaces (produces):**

```cpp
// SensorHash.h (header-only, mirrored in CamSimSensorCommon.ush)
namespace CamSimHash
{
	inline uint32 Pcg(uint32 V) { uint32 S = V * 747796405u + 2891336453u; uint32 W = ((S >> ((S >> 28u) + 4u)) ^ S) * 277803737u; return (W >> 22u) ^ W; }
	inline uint32 Hash(uint32 X, uint32 Y, uint32 Frame, uint32 Seed, uint32 Stream)
	{ return Pcg(X ^ Pcg(Y ^ Pcg(Frame ^ Pcg(Seed ^ Pcg(Stream))))); }
	inline float Uniform(uint32 H) { return (float(H >> 8) + 0.5f) * (1.0f / 16777216.0f); }   // (0,1)
	inline float Gaussian(uint32 X, uint32 Y, uint32 Frame, uint32 Seed, uint32 Stream)
	{
		const float U1 = Uniform(Hash(X, Y, Frame, Seed, Stream * 2u));
		const float U2 = Uniform(Hash(X, Y, Frame, Seed, Stream * 2u + 1u));
		return FMath::Sqrt(-2.0f * FMath::Loge(U1)) * FMath::Cos(2.0f * PI * U2);
	}
	// Stream ids: 1 PRNU, 2 shot, 3 DSNU, 4 read, 5 temporal (bolometer), 6 pixel FPN, 7 column FPN, 8 row FPN, 9 defects.
	// Fixed-pattern streams pass Frame = 0xFFFFFFFF.
	constexpr uint32 FixedFrame = 0xFFFFFFFFu;
}
// SensorReference.h — stage functions (planar float images, W*H [*3 for RGB])
namespace CamSimSensorRef
{
	/** Detector + ADC for one channel: electrons or signal fraction in, DN (float, integer-valued) out. */
	float DetectPixel(float Signal, int32 X, int32 Y, uint32 Channel, const FSensorFrameParams& P);
	/** Display: normalised DN (DN/AdcMax) -> [0,1] output (EO knee+OETF per channel; IR AGC + polarity). */
	TArray<float> DetectImage(const TArray<float>& SignalRgbOrMono, int32 W, int32 H, int32 Channels, const FSensorFrameParams& P);
}
```

Photon (`DetectorType 0`), for channel c (stream ids offset by `c * 16` so RGB channels are independent):

```
e  = Signal * PhotonGain * FullWellE                      (Signal = scene-linear channel after optics)
e1 = e * (1 + Prnu * G(x,y,Fixed,1))
e2 = e1 + sqrt(max(e1, 0)) * G(x,y,Frame,2)
e3 = e2 + DarkE + DsnuE * G(x,y,Fixed,3)
e4 = e3 + ReadNoiseE * G(x,y,Frame,4)
e5 = clamp(e4, 0, FullWellE) * AnalogGain
DN = clamp(floor(e5 * AdcMax / FullWellE + 0.5), 0, AdcMax)
```

Microbolometer (`DetectorType 1`, mono):

```
v  = Signal * PhotonGain + TemporalNoise*G(x,y,Frame,5) + PixelFpn*G(x,y,Fixed,6)
     + ColumnFpn*G(x,0,Fixed,7) + RowFpn*G(0,y,Fixed,8)
DN = clamp(floor(v * AdcMax + 0.5), 0, AdcMax)
```

Defects (both, per pixel, stream 9, fixed): `u = Uniform(Hash(x,y,Fixed,Seed,9))`; `u < HotFraction` → DN = AdcMax; `u > 1 − DeadFraction` → DN = 0.

Display: `n = DN / AdcMax`; EO: `Oetf709(Knee(n, KneeStart))` per channel; IR: `v = saturate(n * DisplayGain + DisplayOffset)`, black-hot `1 − v`.

- [ ] **Step 1: Write the physics tests** in `Tests/SensorPhysicsTest.cpp` (all on `DetectPixel`/`DetectImage`, 256×256 flat fields unless stated):
  - `CamSim.Sensor.Physics.PhotonTransfer` — for `e ∈ {50, 200, 1000, 3000, 6000, 9000}` electrons (set `PhotonGain = e/FullWellE`, `Signal = 1`, `AnalogGain = 1`, `AdcMax = 65535` to make quantisation negligible, defects 0): two frames (FrameIndex 1, 2); temporal variance in electrons `= var(DN1 − DN2)/2 · (FullWellE/AdcMax)²` within 5% of `e + ReadNoiseE²`; fixed-pattern variance of `(DN1 + DN2)/2` minus temporal/2 within 5% of `(Prnu·e)² + DsnuE²` (use `Prnu = 0.01`, `DsnuE = 5`, `ReadNoiseE = 3`).
  - `…Physics.Determinism` — same params + frame → bit-identical; frames 1 vs 2 temporal residual correlation |ρ| < 0.05; fixed pattern (frame-averaged over 16 frames) correlation between two frame sets > 0.95; seed 1 vs 2 fixed pattern correlation |ρ| < 0.05.
  - `…Physics.HashGaussianStatistics` — seed 0, frame 0, stream 0 over 256×256: mean |μ| < 0.01, σ within 1% of 1, |skew| < 0.05, kurtosis within 0.1 of 3.
  - `…Physics.MicrobolometerFpn` — `TemporalNoise = 0`, `PixelFpn = 0`, `ColumnFpn = 0.002`, `RowFpn = 0.001`, `Signal·PhotonGain = 0.5`, `AdcMax = 65535`: std of column means / AdcMax within 10% of 0.002; row means within 10% of 0.001; changing Signal to 0.2 leaves the residual std unchanged (no signal dependence) within 2%.
  - `…Physics.DefectFractions` — 512×512, `HotFraction = DeadFraction = 1e-3`: hot count and dead count each within 4σ of the binomial mean (262 ± 65).
  - `…Physics.ClipsAtFullWellAndZero` — `Signal·PhotonGain = 3` (3× full well) → every DN == AdcMax except dead pixels; `Signal = 0` with `DsnuE = 0`, `DarkE = 0`, `ReadNoiseE = 2` → DN ≥ 0, no NaN, mean ≥ 0.
  - `…Physics.AnalogGainAmplifiesNoise` — same `e = 20`, `AnalogGain` 1 vs 8: temporal SNR (mean/std of DN difference) equal within 10% (analog gain amplifies signal and noise alike; SNR falls with fewer electrons, not with gain) and mean DN scales 8× within 2%.
- [ ] **Step 2: Build** — fails (functions missing).
- [ ] **Step 3: Implement** `SensorHash.h` and the reference stage functions exactly as above; `Run()` (3B.1 entry point) becomes `Optics → Blur → DetectImage → PackNv12` with optics stages as pass-throughs until Task 8 (FocalPx = 0 means off, PsfSigmaPx = 0 means no blur).
- [ ] **Step 4:** physics tests pass; 3B.1 reference tests updated for the new stage (with detector noise zeroed: `Prnu = DsnuE = ReadNoiseE = DarkE = 0` and shot noise present — so compare with tolerance, or set `FullWellE` very large so shot noise ≪ 1 DN: use `FullWellE = 1e9`, `AdcMax = 255`).
- [ ] **Step 5: Commit** — `feat(sensor): reference detector model with photon-transfer physics tests`.

---

### Task 8: Reference optics

**Files:** create `Sensor/SensorOptics.{h,cpp}`; modify `Sensor/SensorReference.{h,cpp}`, `Tests/SensorPhysicsTest.cpp`.

**Interfaces:**

```cpp
namespace CamSimOptics
{
	/** Focal length in pixels for an output width and horizontal FOV. */
	float FocalPx(int32 Width, float HFovDeg);            // (W/2) / tan(HFOV/2)
	/** Ideal (undistorted) normalised radius for a distorted one; false if Newton doesn't converge (|resid| > 1e-5 after 3 iters). */
	bool UndistortRadius(float Rd, float K1, float K2, float& OutRu);
	/** Largest distorted radius in the frame (corner), normalised by focal px. */
	float CornerRadius(int32 W, int32 H, float FocalPx);
	/** PSF sigma in pixels: sqrt((0.42*lambda*N/pitch)^2 + 0.29^2 + extra^2). */
	float PsfSigmaPx(const FSensorOpticsConfig& O);
	/** Normalised 1-D Gaussian taps, radius min(ceil(3 sigma), 8); Taps[0] is the centre. */
	void PsfTaps(float SigmaPx, TArray<float>& OutTaps);
}
namespace CamSimSensorRef
{
	/** Distortion resample + cos^n illumination. Scene (linear RGB, SrcW x SrcH) -> W x H linear RGB; also the noiseless-signal histogram. */
	void Optics(const TArray<FLinearColor>& Scene, int32 SrcW, int32 SrcH, int32 W, int32 H,
		const FSensorFrameParams& P, TArray<FVector3f>& OutRgb, FSensorHistogram& OutHist);
	/** Separable Gaussian with clamp-to-edge. */
	void Blur(TArray<FVector3f>& InOut, int32 W, int32 H, float SigmaPx);
}
```

Optics per output pixel `(px, py)`: `xd = (px + 0.5 − W/2) / FocalPx`, `yd = (py + 0.5 − H/2) / FocalPx`, `rd = hypot`; `ru` by Newton (`r ← r − (r(1+K1 r²+K2 r⁴) − rd)/(1 + 3K1 r² + 5K2 r⁴)`, start `r = rd`, 3 iterations); `s = ru/rd` (1 at rd = 0); source pixel `(xd·s·FocalPx + SrcW/2 − 0.5, yd·s·FocalPx·(SrcH/H) … )` — with `SrcW == W` in production: `sx = xd·s·FocalPx + W/2 − 0.5`, `sy = yd·s·FocalPx + H/2 − 0.5`, bilinear, out of bounds → 0; illumination `cosθ = 1/sqrt(1 + (xd·s)² + (yd·s)²)`, multiply by `cosθ^VignettingExponent`. Sanitize/InputScale as in 3B.1 before sampling. Histogram of `dot(rgb, SignalWeights)` after illumination. `FocalPx == 0` → identity mapping, no illumination.

- [ ] **Step 1: Failing tests** (`SensorPhysicsTest.cpp`):
  - `…Physics.DistortionMatchesForwardModel` — for K1 = −0.2, K2 = 0.05, FocalPx from 1280 px / 60°: for a 9×9 grid of ideal points `ru`, forward `rd = ru(1+K1ru²+K2ru⁴)`, then `UndistortRadius(rd)` recovers `ru` within 1e-4 (normalised) → < 0.1 px.
  - `…Physics.NewtonConvergesOrConfigRejected` — K1 = −0.3 at 60° HFOV, 1280×720: `UndistortRadius(CornerRadius(...))` returns true; K1 = −1.0: returns false (Task 10 turns false into a config error).
  - `…Physics.VignettingIsCosN` — flat field 1.0, `VignettingExponent = 4`, no distortion: corner pixel value within 1% of `cos⁴θ_corner`, edge midpoints likewise.
  - `…Physics.PsfEdgeSpread` — vertical step edge (0 | 1), `Blur` with σ = 1.3 px: fit the edge spread (erf) → σ within 5% of 1.3.
  - `…Physics.OpticsAcrossZoomRange` — HFOV 1° and 60° at 1920 px: `FocalPx` finite (> 0), corner cos⁴θ in (0,1], `PsfSigmaPx` identical for both (depends only on optics config).
  - `…Physics.PsfSigmaFromDatasheet` — eo_hd_cmos: σ = sqrt((0.42·0.55·4/2.9)² + 0.29²) = 0.431 px ± 0.001; mwir_cooled: sqrt((0.42·4·4/15)² + 0.29²) = 0.535 ± 0.001.
- [ ] **Step 2–4:** fail, implement, pass. `Run()` now uses `Optics` + `Blur` (σ = `P.PsfSigmaPx`, skipped when 0).
- [ ] **Step 5: Commit** — `feat(sensor): reference optics (distortion, cos^n, PSF) with physics tests`.

---

### Task 9: GPU passes

**Files:** `Shaders/Private/CamSimSensor.usf`, create `Shaders/Private/CamSimSensorCommon.ush`, `CamSimShaders/Private/SensorGraph.cpp`, `Public/SensorGraph.h`, `Tests/SensorGpuTest.cpp`, `scripts/run_gpu_tests.sh`.

**Passes:** `OpticsCS` (8×8: distortion resample of SceneColor [+ bloom], sanitize, ×scale, cos^n, histogram via groupshared integer atomics → `LinearRgb` PF_FloatRGBA) → `BlurHCS`, `BlurVCS` (row/column tiles in groupshared, taps from a constant array of 9 floats = radius ≤ 8, skipped when σ = 0) → `DetectorCS` (per pixel: detector + ADC + defects + display → `SensorRgb`) → `PackNv12CS` (unchanged).

`CamSimSensorCommon.ush` holds the HLSL twins of `CamSimHash` (`Pcg`, `Hash`, `Uniform`, `Gaussian` with `log`/`cos`/`sqrt`), `Knee`, `Oetf709`, `BinOf`, and `Sanitize` using the bit test `((asuint(V) & 0x7fffffffu) > 0x7f800000u) ? 0 : clamp(V, 0, 65504)`. Rounding helpers use `floor(x + 0.5)`. Bilinear scene/bloom samples clamp their UV to `[rectMin + 0.5 texel, rectMax − 0.5 texel]`.

- [ ] **Step 1: Failing GPU tests** (extend `SensorGpuTest.cpp`; `RunOnGpu` gains the new params; `Compare` unchanged):
  - `CamSim.GPU.Sensor.DetectorMatchesReference` — 64×32 log gradient, eo_hd_cmos detector, FrameIndex 7, Seed 3.
  - `…IrMwirMatchesReference`, `…IrBolometerMatchesReference` (column/row FPN visible).
  - `…OpticsMatchesReference` — K1 = −0.1, vignetting 4, σ = 1.2, 64×32.
  - `…PartialThreadGroups` — 68×34 output (68 % 8 ≠ 0, 34/2 = 17 blocks).
  - `…BloomMatchesReference` — scene + a constant bloom texture at half resolution with its own view rect (the reference adds bloom the same way).
  - `…SanitizesNaNInfNegative` stays.
- [ ] **Step 2:** fails (params/passes missing).
- [ ] **Step 3:** implement the passes; the reference and HLSL must follow the same order (update the order-of-operations comment in `SensorReference.h`: sanitize → scale → [+bloom] → distortion sample → cos^n → histogram → blur → detector → ADC → defects → display → NV12).
- [ ] **Step 4:** `scripts/run_gpu_tests.sh` all pass (Y ≤ 1, UV ≤ 2, histogram exact); NullRHI suite; add `mkdir -p` is already there.
- [ ] **Step 5: Commit** — `feat(sensor): GPU optics, blur and detector passes`.

---

### Task 10: Wire parameters and validation

**Files:** `Camera/CamSimCaptureComponent.cpp` (`UpdateSensorParams`), `Camera/CamSimCamera.cpp` (live FOV), `Config/CamSimConfig.cpp` (`Validate()`), `Tests/SensorConfigTest.cpp`, `Tests/RenderPathTest.cpp` (frame stats).

- [ ] **Step 1: Failing tests:** `CamSim.Sensor.Config.DistortionMustConverge` — `sensor_modes.eo.optics.k1: -1.0` at the default HFOV/capture size → `Validate()` error naming `k1` ("does not converge at the frame corner"); `-0.3` → no error.
- [ ] **Step 2–3:** `Validate()` calls `CamSimOptics::UndistortRadius(CornerRadius(CaptureWidth, CaptureHeight, FocalPx(CaptureWidth, HFovDeg)), K1, K2)` per mode; also warn (log once, not an error) at runtime if a zoom FOV makes it fail. `UpdateSensorParams` (signature gains `float LiveHFovDeg`) fills every `FSensorFrameParams` field: detector from the mode config, `DarkE = DarkCurrentEs / FrameRate`, `AdcMax = (1 << AdcBits) − 1`, `FocalPx = CamSimOptics::FocalPx(CaptureWidth, LiveHFovDeg)`, `K1/K2`, `VignettingExponent`, `PsfSigmaPx = CamSimOptics::PsfSigmaPx(Optics)`, `Seed`, `FrameIndex` (the controller's serial). The camera passes `SceneCapture->FOVAngle`.
- [ ] **Step 4:** tests pass; live smoke `python3 scripts/bench/run_bench.py --smoke --label t10-smoke` → `/snapshot/sensor` shows noise at night and a sharp daylight image; `ci_validate --native` passes.
- [ ] **Step 5: Commit** — `feat(sensor): wire the physical sensor model into the live pipeline`.

---

### Task 11: Calibrate, measure, document

- [ ] **Step 1: Bench.** Full bench at 720p and 1080p (1080p via a temporary capture-size yaml edit, reverted), Yosemite snap test (scratch script in `.superpowers` notes or re-create: 90° snaps, 0% coarse), `ci_validate --native`.
- [ ] **Step 2: Calibrate** EO `target_grey`/`max_photon_gain_ev` and IR photon gain targets only if the exit-criteria lumas miss (daylight EO mean luma 90–170, < 1% clipped; night EO darker than day with higher temporal noise; IR column striping present and < 2 DN only for `lwir_uncooled` — for the default `mwir_cooled`, residual FPN < 1 DN). Measure temporal noise from two consecutive `/snapshot/sensor` shots.
- [ ] **Step 3: Verify** sensor graph GPU p95 ≤ 2 ms at 1080p; 30 fps, 0 dropped; frame p95 ≤ post-crossfade baseline + 1 ms.
- [ ] **Step 4: Shots.** New set in `scripts/bench/shots/macos/3b2/` (EO 9 poses + IR nadir/dusk/night); baselines `macos-m1pro-3b2-{720p,1080p}.json`.
- [ ] **Step 5: Docs.** ROADMAP 3B.2 results (table, exit criteria, removals), `docs/configuration.md` (presets, parameter table, removed keys), CLAUDE.md (architecture tree: `Overlay/` gone, `SensorPostProcess` gone; test counts; gotchas: one sensor path, presets, physics tests), then ask the user for visual review (the controller does this, not the implementer).
- [ ] **Step 6: Commit** — `docs: ROADMAP 3B.2 results (physical sensor model)`.
