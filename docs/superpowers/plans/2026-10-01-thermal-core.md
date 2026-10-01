# Thermal Core (ROADMAP 4A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** IR mode stops being the luminance of the visible image: every pixel gets an in-band (MWIR or LWIR) radiance from a surface temperature and emissivity, fed through the unchanged Milestone 3 optics → detector → ADC → AGC chain, so night IR works, vehicles read white-hot, water/land cross over and shadows are cool.

**Architecture:** On the game thread `FThermalFrameBuilder` evaluates a closed-form diurnal surface temperature per material class (`FThermalModel`, Fourier response of a semi-infinite solid), the sky (`FThermalSky`), an in-band Planck LUT (`FBandRadiance`) and a 256-entry stencil → class/offset table from the entity manager into the POD `FThermalFrameParams`. On the render thread, in IR mode, `ThermalCS` runs at `ReplacingTonemapper` before `SensorCS`: per pixel it classifies sky / visible entity / water / terrain from scene depth, custom depth + stencil, GBuffer base colour and scene colour, adds a per-pixel solar "fast term" recovered from the EO render, applies emissivity, sky reflection and path radiance, and writes R32F radiance. `SensorCS` consumes it as a radiance input (no pre-exposure division, no bloom, signal weights (1, 0, 0), signal = L / B(300 K)) with its own thermal AE slot and a capped AGC. `CamSimThermalRef` is the CPU reference `ThermalCS` mirrors expression for expression.

**Tech Stack:** UE 5.8 C++ (RDG, global compute shaders, UE Automation tests), HLSL (`/CamSim` virtual dir, Metal + Vulkan portable), Python 3.10+ (live acceptance script, numpy + pillow).

**Spec:** `docs/superpowers/specs/2026-10-01-thermal-core-design.md`

## Global Constraints

- Copyright header `// Copyright CamSim Contributors. All Rights Reserved.` on every new C++/HLSL file; Python files start with the repo's usual docstring.
- UE naming (`A`/`U`/`F`/`E`/`I` prefixes), PascalCase, verb-first functions; `#include "CoreMinimal.h"` first, `.generated.h` last; forward declarations preferred in headers.
- `ThermalCS` mirrors `CamSimThermalRef::EvaluatePixel` expression for expression (same order, same constants, same float types); change both together. `CamSim.GPU.Thermal.MatchesCpu` holds them to 1e-4 relative radiance.
- Rounding (where any) is `floor(x + 0.5)`, never HLSL `round`. Non-finite tests are bit tests (`asuint`), never `x != x` (Metal fast-math folds it).
- Plain compute only: no float atomics, no wave intrinsics, no groupshared in `ThermalCS` (portable to Vulkan).
- Targets stay on `BuildSettingsVersion.V7`; no new modules or plugins (new code lives in the existing `CamSimShaders` and `CamSimTest` modules).
- Sensor/thermal state uses `FSimClock` sim time, never `DeltaTime`. Altitudes are WGS-84 ellipsoid heights; 1 UE unit = 1 cm.
- New config keys are documented in `docs/configuration.md` and `deploy/camsim_config.yaml` in the same task that adds them; `CamSim.Config.CanonicalConfigHasNoUnknownKeys` must keep passing.
- `thermal.enabled: false` (or EO mode) leaves the 3B.2 path bit-for-bit unchanged: every existing `CamSim.*` and `CamSim.GPU.*` test keeps passing.
- Performance: `ThermalCS` ≤ 0.5 ms p95 at 1080p on the M1 Pro (measured with an `FGPUStat` scope, never `RQT_AbsoluteTime`); builder < 50 µs per frame for ≤ 32 classes.
- NullRHI tests via the headless command below (macOS needs `-DisablePython`); GPU tests (`CamSim.GPU.*`) via `scripts/run_gpu_tests.sh <filter>`.
- Keep `ROADMAP.md` up to date as tasks land; record any editor/human change (assets, materials, textures) in `ROADMAP.md` (4A needs none — say so explicitly).
- Commit messages end with the two lines:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
  ```

## Commands

```bash
# Build (editor target). --mode editor is required on macOS: run.sh switches to packaged mode
# (a full BuildCookRun) when Saved/StagedBuilds/Mac/CamSimTest.app exists. Runs
# Engine/Build/BatchFiles/Mac/Build.sh CamSimTestEditor Mac Development (Linux: .../Linux/Build.sh ... Linux ...).
set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5

# NullRHI automation tests: run_tests <Filter>, e.g. run_tests CamSim.Thermal
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
```

Every "Run" step below means: build, then `run_tests <filter>` (or `scripts/run_gpu_tests.sh <filter>`). A step that expects a compile failure stops at the build.

## Review Focus

Input classes and failure modes the spec's tests do not cover, each pinned by a test in its owning task:

1. **Flat or narrow thermal histograms** (uniform night terrain, a single dominant class, a bimodal land/water frame): the thermal AGC must stay finite, respect `agc_max_display_gain`, and never crush half the frame to black. → `CamSim.Sensor.Controller.ThermalAgcFlatSceneCapped` (Task 10).
2. **View rays below the horizon and a camera at or below the sea surface** (sky pixels with negative elevation from altitude; a deck camera in a wave trough): sky emissivity clamps, water classification and the curvature term stay finite. → `CamSim.Thermal.Reference.SkyBelowHorizonAndCameraAtSeaLevel` (Task 7).
3. **Suns that never rise or set, and local solar time across the date line** (lat 80° in June/December; lon ±179.9° near UTC midnight): the Fourier fit, class temperatures and day-of-year stay finite and in range. → `CamSim.Thermal.Model.PolarDayAndNight` (Task 6) and `CamSim.Thermal.Builder.DateLineLocalTime` (Task 9).
4. **A reused stencil value or an unknown `thermal_material`** (an entity destroyed and its stencil handed to a different type 4 frames later; a typo in `entity_types`): the table is rebuilt from live entities every frame, an unknown name falls back to `vehicle_paint` with one warning, and no class index ≥ `NumClasses` ever reaches the shader. → `CamSim.Thermal.Builder.StencilTableReuseAndUnknownMaterial` (Task 9).
5. **Fast-term extremes** (black base colour, emissive or specular highlights, night with S = 0, `K_lum` = 0, offsets that push T past the LUT range): no NaN/Inf, clamps hold, radiance saturates at B(1000 K) / B(150 K). → `CamSim.Thermal.Reference.FastTermExtremesFinite` (Task 7).

## Decisions (where this plan completes or departs from the spec)

- **K_lum source:** the atmosphere sun light (`UDirectionalLightComponent` with `bAtmosphereSunLight`, index 0, brightest) — intensity × luminance of its colour × luminance of `USkyAtmosphereComponent::GetAtmosphereTransmitanceOnGroundAtPlanetTop` × sin(sun elevation) — divided by the class model's reference irradiance S_clear · (1 − 0.75 c^3.4). An unshadowed horizontal surface of the class albedo then gives S_abs,pix = S_abs,ref exactly (tested); cloud-shadowed pixels come out cooler.
- **AE:** a separate `sensor_modes.ir.thermal_exposure` block (defaults −8 / 0 EV, target grey 0.5) and its own AE state slot (seed −1 EV) instead of re-tuning `exposure`, which the `thermal.enabled: false` A/B path still needs.
- **UE scene exposure in thermal mode** is fixed at −12 EV (`AutoExposureBias`), because the radiance AE gain would otherwise drive `View.PreExposure` and push the EO scene colour (the fast term's input) out of fp16 range.
- **AGC:** the cap (`agc_max_display_gain`, flat key next to the other `agc_*` keys) applies to the radiance path only; the thermal AGC maps its percentile band to [0.1, 0.9] (a fixed 0.1 margin) and interpolates percentiles inside histogram bins. Without the margin a bimodal night frame (land/water, no texture in 4A) puts half the pixels exactly on the low percentile, i.e. black.
- **Sea "sphere":** evaluated camera-relative to second order (`h = H_cam + v + d²/2R`, R = Gaussian radius at the camera) instead of |P − C| − R with a centre 6.4e8 cm away, which float32 resolves only to ~64 cm.
- **Sky reflection:** L_sky,hemi is the view-factor (cosine-weighted) average of the in-band sky radiance B(T_sky(el)), consistent with the sky pixels; the broadband forcing uses the cosine-weighted ε.
- **Non-finite inputs:** a NaN/Inf scene colour or depth makes the whole pixel B(T_air) (no path term).
- **`air_diurnal_swing_k` is peak-to-peak** (A_air = swing / 2); `thermal.air_temperature_c` is the daily mean.
- **Keys the spec assumed but the code lacks:** `thermal.air_temperature_c`, `ocean.water_temperature_c` (initial `FOceanSurface` water temperature until CIGI Maritime Surface sets it), `detector.band_lo_um`/`band_hi_um`; env `CAMSIM_IR_PRESET` (re-applies an IR preset after yaml, for the MWIR/LWIR acceptance runs) and `CAMSIM_CAPTURE_WIDTH`/`HEIGHT` (1080p perf run).
- **Entity stencil tagging** also turns on when thermal is available (`UCamSimSubsystem::IsEntityStencilTaggingEnabled`), not only with ML ground truth.
- **Environment plumbing:** the atmospheric snapshot now carries the CIGI Atmosphere Control air temperature (it never did), cloud cover and whether fog is active.
- **Built-in classes:** `terrain_default`, `water` (fixed), `vehicle_paint`, `asphalt`, `vegetation`, `concrete`; water's fixed ±0.5 K swing peaks at 15:00 local solar.
- **Acceptance date** 21 December: solar noon at San Francisco is at 28.8° elevation (truck shadow ~5 m long) and the night land/water flip is ~3 K (in June the 02:00 soil is within 1 K of 15 °C water). Check (e) uses a 90° FOV view pitched +20°, because the Swinbank angular model makes the near-horizon sky ≈ T_air, warmer than cooled night terrain.

---

## File Structure

| File | Responsibility |
|---|---|
| Create `Source/CamSimShaders/Public/ThermalFrameParams.h` | `FThermalFrameParams` POD (LUT, classes, stencil table, sky, atmosphere, solar, geometry) + `ThermalLutRadiance` |
| Create `Source/CamSimShaders/Public/ThermalPass.h`, `Private/ThermalPass.cpp` | `FThermalPassInputs`, `AddThermalPass`, `IsThermalPassSupported`, spike constants |
| Create `Shaders/Private/CamSimThermal.usf`, `CamSimThermalCommon.ush` | `ThermalCS` and its per-pixel maths |
| Create `Source/CamSimTest/Thermal/ThermalTypes.h` | `EThermalTemperatureSource`, `FThermalMaterialSpec` (config-facing) |
| Create `Source/CamSimTest/Thermal/BandRadiance.h/.cpp` | `FBandRadiance`: in-band Planck integral + LUT |
| Create `Source/CamSimTest/Thermal/ThermalMaterials.h/.cpp` | `FThermalMaterial`, `FThermalMaterialTable` (built-ins + overrides) |
| Create `Source/CamSimTest/Thermal/ThermalSky.h/.cpp` | `FThermalSky`: Swinbank + angular emissivity + cloud blend |
| Create `Source/CamSimTest/Thermal/ThermalModel.h/.cpp` | `FThermalSite`, `FThermalModel`: closed-form class temperatures |
| Create `Source/CamSimTest/Thermal/ThermalReference.h/.cpp` | `CamSimThermalRef`: CPU reference of `ThermalCS` |
| Create `Source/CamSimTest/Thermal/ThermalFrameBuilder.h/.cpp` | `FThermalStencilEntity`, `FThermalFrameInputs`, `FThermalFrameBuilder` |
| Create `Source/CamSimTest/Thermal/ThermalFrameSources.h/.cpp` | `CamSimThermal::{ResolveStencilEntity, SunIlluminanceLux, GatherFrameInputs}` |
| Modify `Source/CamSimTest/Config/CamSimConfig.h/.cpp` | `thermal:` section, sensor/ocean keys, env vars, validation |
| Modify `Source/CamSimTest/Sensor/SensorTypes.h`, `SensorPresets.cpp` | band keys, `ThermalExposure`, `AGCMaxDisplayGain` |
| Modify `Source/CamSimTest/Sensor/SensorController.h/.cpp` | radiance AE slot, thermal exposure, interpolated percentiles, AGC cap + margin |
| Modify `Source/CamSimShaders/Public/SensorGraph.h`, `Private/SensorGraph.cpp` | `bRadianceInput` |
| Modify `Source/CamSimTest/Entity/EntityTypeTable.h/.cpp` | `thermal_material`, `thermal_offset_k` |
| Modify `Source/CamSimTest/Entity/CamSimEntity.h`, `CamSimEntityManager.h/.cpp` | `IsSurfaceVehicle`, `GetThermalStencilEntities`, tagging gate |
| Modify `Source/CamSimTest/Environment/CamSimEnvironment.h/.cpp` | snapshot air temp / cloud / fog, `FoldAtmosphere`, `FoldWeather` |
| Modify `Source/CamSimTest/Subsystem/CamSimSubsystem.h/.cpp` | `IsThermalAvailable`, `IsEntityStencilTaggingEnabled`, initial water temperature |
| Modify `Source/CamSimTest/Camera/SensorGpuTimer.h/.cpp`, `CamSimFrameStats.h/.cpp`, `CamSimCamera.cpp` | thermal GPU stat, `thermal_gpu_ms` |
| Modify `Source/CamSimTest/Camera/CamSimCaptureComponent.h/.cpp`, `CamSimFrameGrabExtension.h/.cpp` | build/send thermal params; run `ThermalCS` in IR |
| Create `Tests/ThermalPlanckTest.cpp`, `ThermalConfigTest.cpp`, `ThermalSensorConfigTest.cpp`, `ThermalSkyTest.cpp`, `ThermalModelTest.cpp`, `ThermalTestScene.h`, `ThermalReferenceTest.cpp`, `ThermalGpuTest.cpp`, `ThermalBuilderTest.cpp`, `ThermalSourcesTest.cpp`; modify `SensorControllerTest.cpp`, `SensorGpuTest.cpp`, `RenderPathTest.cpp` | tests |
| Create `scripts/thermal_check.py` | live acceptance |
| Modify `deploy/camsim_config.yaml`, `docs/configuration.md`; create `docs/thermal.md`; modify `docs/architecture.md`, `ROADMAP.md`, `CLAUDE.md` | docs |

All `Source/...`, `Shaders/...`, `Tests/...` paths are under `unreal_project/CamSimTest/` (`Tests/` = `Source/CamSimTest/Tests/`).

---

### Task 1: Spike — is GBuffer base colour readable at the tonemapper with Substrate?

The fast solar term divides scene luminance by the GBuffer base colour luminance. With `r.Substrate=True` and `r.Substrate.ProjectGBufferFormat=0` (Substrate's blendable GBuffer, `Config/DefaultEngine.ini`) that texture is unverified at `ReplacingTonemapper`. This task probes it (and the atmosphere sun light `K_lum` reads) on a live frame, records the outcome, and reverts the probe.

**Files:**
- Modify (throwaway, reverted in Step 6): `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameGrabExtension.cpp`, `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCaptureComponent.cpp`
- Modify (kept): `ROADMAP.md`

**Interfaces:**
- Consumes: `FPostProcessMaterialInputs::SceneTextures` (`FSceneTextureUniformParameters::{GBufferCTexture, SceneDepthTexture}`), `FRHIGPUTextureReadback`, `AddEnqueueCopyPass(FRDGBuilder&, FRHIGPUTextureReadback*, FRDGTextureRef)`, `UDirectionalLightComponent::{IsUsedAsAtmosphereSunLight, GetAtmosphereSunLightIndex, Intensity, GetLightColor}`, `USkyAtmosphereComponent::GetAtmosphereTransmitanceOnGroundAtPlanetTop`.
- Produces: the **decision record** in `ROADMAP.md` (4A section), one of:
  - `LINEAR` — GBufferC holds linear base colour. Task 8 sets `CamSimThermalPass::bBaseColorAtTonemapper = true`, `bBaseColorSrgbEncoded = false`.
  - `SRGB_ENCODED` — GBufferC holds base colour, gamma-encoded in a non-sRGB format. Task 8 sets `true` / `true` (ThermalCS decodes).
  - `FALLBACK` — not readable or not meaningful. Task 8 sets `false` / `false`: the fast term is off (`k_fast = 0`, `FThermalFrameParams::KFastScale = 0`) and `UCamSimSubsystem` logs one warning (Task 13). Everything else is unchanged.
  - plus the name, intensity and transmittance of the atmosphere sun light Task 13 will read (`K_lum`); if none is found, Task 13's `GatherFrameInputs` leaves `SunIlluminanceLux = 0`, which the builder turns into `KFastScale = 0` (same fallback).

- [ ] **Step 1: Write the probe (render thread).** In `CamSimFrameGrabExtension.cpp` add below the includes:

```cpp
// ---- ROADMAP 4A Task 1 spike (throwaway: reverted at the end of the task) ----
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
static TAutoConsoleVariable<int32> CVarCamSimThermalSpike(TEXT("camsim.ThermalSpike"), 0,
	TEXT("4A spike: probe GBufferC (base colour) at ReplacingTonemapper and log the verdict"));
namespace CamSimThermalSpike
{
	TUniquePtr<FRHIGPUTextureReadback> Readback;
	FIntPoint Extent = FIntPoint::ZeroValue;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
	EPixelFormat Format = PF_Unknown;
	bool bSrgbFlag = false;
	int32 State = 0;   // 0 idle, 1 copy queued, 2 done
	int32 FramesWaited = 0;

	void Probe(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
	{
		if (State == 2 || CVarCamSimThermalSpike.GetValueOnRenderThread() == 0) return;
		if (State == 0)
		{
			if (++FramesWaited < 300) return;   // ~10 s: let tiles and TSR history settle
			const FSceneTextureUniformParameters* St = Inputs.SceneTextures.SceneTextures
				? Inputs.SceneTextures.SceneTextures->GetParameters().GetContents() : nullptr;
			if (!St || !St->GBufferCTexture || !St->SceneDepthTexture)
			{
				UE_LOG(LogCamSim, Display, TEXT("THERMAL SPIKE: VERDICT FALLBACK (scene-texture UB %s, GBufferC %s)"),
					St ? TEXT("present") : TEXT("missing"), (St && St->GBufferCTexture) ? TEXT("present") : TEXT("missing"));
				State = 2;
				return;
			}
			const FRDGTextureDesc& D = St->GBufferCTexture->Desc;
			const FRDGTextureDesc& Dd = St->SceneDepthTexture->Desc;
			Extent = D.Extent;
			ViewSize = View.UnscaledViewRect.Size();
			Format = D.Format;
			bSrgbFlag = EnumHasAnyFlags(D.Flags, TexCreate_SRGB);
			UE_LOG(LogCamSim, Display, TEXT("THERMAL SPIKE: GBufferC %dx%d %s srgb=%d; SceneDepth %dx%d; view %dx%d"),
				D.Extent.X, D.Extent.Y, GetPixelFormatString(D.Format), bSrgbFlag ? 1 : 0, Dd.Extent.X, Dd.Extent.Y,
				ViewSize.X, ViewSize.Y);
			if (D.Extent != Dd.Extent || (D.Format != PF_B8G8R8A8 && D.Format != PF_R8G8B8A8))
			{
				UE_LOG(LogCamSim, Display, TEXT("THERMAL SPIKE: VERDICT FALLBACK (extent mismatch or unexpected format; a dummy texture?)"));
				State = 2;
				return;
			}
			Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("CamSimThermalSpike"));
			AddEnqueueCopyPass(GraphBuilder, Readback.Get(), St->GBufferCTexture);
			State = 1;
			return;
		}
		if (!Readback->IsReady()) return;
		int32 Pitch = 0;
		const uint8* Px = static_cast<const uint8*>(Readback->Lock(Pitch));
		const int32 W = FMath::Min(ViewSize.X, Extent.X), H = FMath::Min(ViewSize.Y, Extent.Y);
		TArray<float> Lums;
		TArray<uint8> Pgm;
		const FTCHARToUTF8 Header(*FString::Printf(TEXT("P5\n%d %d\n255\n"), W, H));
		Pgm.Append(reinterpret_cast<const uint8*>(Header.Get()), Header.Length());
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const uint8* P = Px + (static_cast<int64>(Y) * Pitch + X) * 4;
				const float R = (Format == PF_B8G8R8A8 ? P[2] : P[0]) / 255.0f, G = P[1] / 255.0f, B = (Format == PF_B8G8R8A8 ? P[0] : P[2]) / 255.0f;
				const float L = 0.2126f * R + 0.7152f * G + 0.0722f * B;
				Pgm.Add(static_cast<uint8>(FMath::Clamp(FMath::FloorToInt32(L * 255.0f + 0.5f), 0, 255)));
				if (L > 0.0f) Lums.Add(L);
			}
		}
		Readback->Unlock();
		const FString PgmPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("thermal_spike_basecolor.pgm"));
		FFileHelper::SaveArrayToFile(Pgm, *PgmPath);
		Lums.Sort();
		double Mean = 0.0, Var = 0.0;
		int32 InRange = 0;
		for (float L : Lums) { Mean += L; InRange += (L >= 0.01f && L <= 0.95f) ? 1 : 0; }
		Mean /= FMath::Max(1, Lums.Num());
		for (float L : Lums) Var += (L - Mean) * (L - Mean);
		const double Std = FMath::Sqrt(Var / FMath::Max(1, Lums.Num()));
		const float Median = Lums.Num() > 0 ? Lums[Lums.Num() / 2] : 0.0f;
		const double NonZero = double(Lums.Num()) / FMath::Max(1, W * H);
		const double InFrac = double(InRange) / FMath::Max(1, Lums.Num());
		const bool bMeaningful = NonZero >= 0.5 && InFrac >= 0.9 && Std >= 0.01;
		const TCHAR* Verdict = !bMeaningful ? TEXT("FALLBACK")
			: (Median >= 0.02f && Median <= 0.35f) ? TEXT("LINEAR")
			: (Median > 0.35f && Median <= 0.6f && !bSrgbFlag) ? TEXT("SRGB_ENCODED") : TEXT("FALLBACK");
		UE_LOG(LogCamSim, Display, TEXT("THERMAL SPIKE: non-zero %.3f, in [0.01,0.95] %.3f, mean %.3f, std %.3f, median %.3f -> VERDICT %s (image: %s)"),
			NonZero, InFrac, Mean, Std, Median, Verdict, *PgmPath);
		State = 2;
	}
}
// ---- end spike ----
```

and as the first statement of `FCamSimFrameGrabExtension::RunSensor_RenderThread`:

```cpp
	CamSimThermalSpike::Probe(GraphBuilder, View, Inputs);   // 4A spike (throwaway)
```

- [ ] **Step 2: Write the probe (game thread, `K_lum` inputs).** In `CamSimCaptureComponent.cpp` add the includes `"Components/DirectionalLightComponent.h"`, `"Components/SkyAtmosphereComponent.h"`, `"UObject/UObjectIterator.h"`, `"HAL/IConsoleManager.h"`, and at the top of `UCamSimCaptureComponent::UpdateSensorParams`:

```cpp
	// ---- ROADMAP 4A Task 1 spike (throwaway) ----
	if (IConsoleVariable* Spike = IConsoleManager::Get().FindConsoleVariable(TEXT("camsim.ThermalSpike")); Spike && Spike->GetInt() != 0)
	{
		static double NextLogSec = 0.0;
		const double Now = FPlatformTime::Seconds();
		if (Now >= NextLogSec)
		{
			NextLogSec = Now + 5.0;
			for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
			{
				if (It->GetWorld() != GetWorld()) continue;
				FLinearColor Tr = FLinearColor::White;
				for (TObjectIterator<USkyAtmosphereComponent> S; S; ++S)
				{
					if (S->GetWorld() == GetWorld()) { Tr = S->GetAtmosphereTransmitanceOnGroundAtPlanetTop(*It); break; }
				}
				UE_LOG(LogCamSim, Display, TEXT("THERMAL SPIKE: light %s owner %s visible=%d atmosphereSun=%d index=%d intensity=%.1f color=%s transmittance=%s"),
					*It->GetName(), *GetNameSafe(It->GetOwner()), It->IsVisible() ? 1 : 0, It->IsUsedAsAtmosphereSunLight() ? 1 : 0,
					It->GetAtmosphereSunLightIndex(), It->Intensity, *It->GetLightColor().ToString(), *Tr.ToString());
			}
		}
	}
	// ---- end spike ----
```

- [ ] **Step 3: Build.** Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: `==> Build complete.`

- [ ] **Step 4: Run the probe on a live frame** (San Francisco, Presidio, nadir, 21 Dec solar noon, EO). Save as a scratch file (not committed), e.g. `$TMPDIR/thermal_spike.py`, and run it from the repo root with `uv run -q python $TMPDIR/thermal_spike.py`:

```python
import dataclasses, os, subprocess, sys, time
from pathlib import Path
REPO = Path.cwd()
sys.path.insert(0, str(REPO / "scripts"))
import dis_vehicle_check as dvc
from bench import run_bench as rb

pose = dataclasses.replace(dvc.nadir_on(dvc.TRUCK, dvc.TRUCK_GROUND_HAE + 330.0, 30.0)(0.0),
                           utc_hour=20, utc_minute=10, month=12, day=21)
host = rb.Host(); host.pose = pose; host.thread.start()
env = dict(os.environ, CAMSIM_MULTICAST_ADDR="127.0.0.1")
subprocess.run([str(REPO / "scripts/run.sh"), "--headless", "--local", "--detach",
                "-ExecCmds=camsim.ThermalSpike 1"], env=env, check=True, stdout=subprocess.DEVNULL)
try:
    rb.wait_ready(REPO / ".cache" / "camsim.pid"); rb.wait_terrain(120); time.sleep(25)
finally:
    host.stop.set(); subprocess.run([str(REPO / "scripts/stop.sh")], check=False)
log = (Path.home() / "Library/Logs/CamSimTest/CamSimTest.log").read_text(errors="replace")
print("\n".join(l for l in log.splitlines() if "THERMAL SPIKE" in l))
```

Expected: one `VERDICT` line and several `light …` lines. Open `unreal_project/CamSimTest/Saved/thermal_spike_basecolor.pgm` (Preview opens PGM): a **base-colour** image shows imagery/material albedo with **no shading and no shadows**; if it shows lit shading, cast shadows or is uniform, the verdict is `FALLBACK` regardless of the log. Note the atmosphere sun light (`atmosphereSun=1 index=0`, the brightest such light) and its intensity (expect ~1e5 lux if the EO AE's −15.5 EV daylight gain is right; 10 lux would mean the level's `ADirectionalLight` driven by `ACamSimEnvironment::ApplySun` is the atmosphere sun).

- [ ] **Step 5: Record the outcome.** In `ROADMAP.md`, insert after the Milestone 4 table (before the `---` that precedes Milestone 5):

```markdown
### 4A Thermal core (in progress)

Spec: `docs/superpowers/specs/2026-10-01-thermal-core-design.md`; plan:
`docs/superpowers/plans/2026-10-01-thermal-core.md`.

- Spike (plan Task 1, 2026-10-01): GBuffer base colour at `ReplacingTonemapper` with
  `r.Substrate=True`, `r.Substrate.ProjectGBufferFormat=0` — verdict **<LINEAR | SRGB_ENCODED | FALLBACK>**
  (GBufferC <W>x<H> <format>, sRGB flag <0/1>; non-zero <x>, in-range <x>, median <x>; the dumped image
  <did / did not> look like unlit albedo). Atmosphere sun light for `K_lum`: `<component name>` on
  `<owner>`, <intensity> lux, ground transmittance <rgb>. Consequence: Task 8 sets
  `CamSimThermalPass::bBaseColorAtTonemapper = <true/false>`, `bBaseColorSrgbEncoded = <true/false>`.
```

replacing every `<…>` with the measured values from Step 4 (this is the decision record later tasks read).

- [ ] **Step 6: Revert the probe and commit the record only.**

```bash
git checkout -- unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameGrabExtension.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCaptureComponent.cpp
git status --short   # only ROADMAP.md modified
git add ROADMAP.md && git commit -F - <<'EOF'
docs(thermal): 4A spike — GBuffer base colour at the tonemapper under Substrate

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 2: In-band Planck radiance and its LUT (`FBandRadiance`)

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalFrameParams.h` (LUT part; Task 7 completes it)
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/BandRadiance.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/BandRadiance.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalPlanckTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct FThermalFrameParams   // ThermalFrameParams.h (CamSimShaders, header-only)
  {
      static constexpr int32 LutSize  = 1024;
      static constexpr float LutMinK  = 150.0f;
      static constexpr float LutMaxK  = 1000.0f;
      static constexpr float LutScale = static_cast<float>(LutSize - 1) / (LutMaxK - LutMinK);
      float LogLut[LutSize] = {};      // ln B(T_i)
  };
  inline float ThermalLutRadiance(const float* LogLut, float TK);

  class CAMSIMTEST_API FBandRadiance
  {
  public:
      static constexpr int32 LutSteps = 512;
      static double SpectralRadiance(double LambdaM, double TK);                               // W m^-2 sr^-1 m^-1
      static double IntegrateBand(double TK, double LoUm, double HiUm, int32 Steps = 4096);    // W m^-2 sr^-1
      void   Build(double LoUm, double HiUm);
      bool   IsBuilt() const;
      double GetLoUm() const;
      double GetHiUm() const;
      float  Radiance(float TK) const;          // ThermalLutRadiance(GetLogLut(), TK)
      const float* GetLogLut() const;
  };
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalPlanckTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/BandRadiance.h"

#include <limits>

// CamSim.Thermal.Planck.*: the in-band Planck integral and its LUT (ROADMAP 4A).

namespace
{
	constexpr double StefanBoltzmann = 5.670374419e-8;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPlanckWholeSpectrumTest, "CamSim.Thermal.Planck.WholeSpectrumIsStefanBoltzmann",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPlanckWholeSpectrumTest::RunTest(const FString& Parameters)
{
	for (const double T : { 200.0, 300.0, 500.0, 1000.0 })
	{
		const double Band = FBandRadiance::IntegrateBand(T, 0.1, 2000.0, 8192);
		const double Expected = StefanBoltzmann * T * T * T * T / UE_DOUBLE_PI;
		TestNearlyEqual(*FString::Printf(TEXT("0.1-2000 um at %.0f K = sigma T^4 / pi within 0.1%%"), T), Band, Expected, Expected * 1e-3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPlanckKnownBandsTest, "CamSim.Thermal.Planck.KnownBandValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPlanckKnownBandsTest::RunTest(const FString& Parameters)
{
	// Reference values (independent Simpson integration, 2026-10-01): 300 K, W m^-2 sr^-1.
	TestNearlyEqual(TEXT("MWIR 3-5 um at 300 K"), FBandRadiance::IntegrateBand(300.0, 3.0, 5.0), 1.86596, 1.86596 * 1e-3);
	TestNearlyEqual(TEXT("LWIR 8-12 um at 300 K"), FBandRadiance::IntegrateBand(300.0, 8.0, 12.0), 38.5004, 38.5004 * 1e-3);
	TestEqual(TEXT("empty band"), FBandRadiance::IntegrateBand(300.0, 5.0, 3.0), 0.0);
	TestEqual(TEXT("0 K"), FBandRadiance::SpectralRadiance(4e-6, 0.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPlanckLutTest, "CamSim.Thermal.Planck.LutMatchesIntegral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPlanckLutTest::RunTest(const FString& Parameters)
{
	struct FBand { double Lo, Hi; const TCHAR* Name; };
	for (const FBand B : { FBand{ 3.0, 5.0, TEXT("MWIR") }, FBand{ 8.0, 12.0, TEXT("LWIR") } })
	{
		FBandRadiance R;
		TestFalse(TEXT("not built"), R.IsBuilt());
		R.Build(B.Lo, B.Hi);
		TestTrue(TEXT("built"), R.IsBuilt());
		TestEqual(TEXT("lo"), R.GetLoUm(), B.Lo);
		double WorstRel = 0.0, WorstT = 0.0;
		// Mid-cell temperatures are the worst case of linear interpolation in ln B.
		const double Cell = (FThermalFrameParams::LutMaxK - FThermalFrameParams::LutMinK) / (FThermalFrameParams::LutSize - 1);
		for (int32 I = 0; I < FThermalFrameParams::LutSize - 1; I += 7)
		{
			const double T = FThermalFrameParams::LutMinK + (I + 0.5) * Cell;
			const double Exact = FBandRadiance::IntegrateBand(T, B.Lo, B.Hi);
			const double Rel = FMath::Abs(R.Radiance(static_cast<float>(T)) - Exact) / Exact;
			if (Rel > WorstRel) { WorstRel = Rel; WorstT = T; }
		}
		TestTrue(*FString::Printf(TEXT("%s LUT within 0.1%% (worst %.5f%% at %.1f K)"), B.Name, WorstRel * 100.0, WorstT), WorstRel <= 1e-3);
		TestNearlyEqual(*FString::Printf(TEXT("%s clamps below the range"), B.Name), R.Radiance(10.0f), R.Radiance(FThermalFrameParams::LutMinK), 0.0f);
		TestNearlyEqual(*FString::Printf(TEXT("%s clamps above the range"), B.Name), R.Radiance(5000.0f), R.Radiance(FThermalFrameParams::LutMaxK), 0.0f);
		TestNearlyEqual(*FString::Printf(TEXT("%s NaN -> LutMinK"), B.Name), R.Radiance(std::numeric_limits<float>::quiet_NaN()), R.Radiance(FThermalFrameParams::LutMinK), 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPlanckMonotonicTest, "CamSim.Thermal.Planck.Monotonic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPlanckMonotonicTest::RunTest(const FString& Parameters)
{
	FBandRadiance R;
	R.Build(3.0, 5.0);
	const float* L = R.GetLogLut();
	int32 Bad = 0;
	for (int32 I = 1; I < FThermalFrameParams::LutSize; ++I) Bad += (L[I] > L[I - 1]) ? 0 : 1;
	TestEqual(TEXT("LUT strictly increasing"), Bad, 0);
	float Prev = R.Radiance(150.0f);
	for (float T = 150.1f; T <= 1000.0f; T += 0.37f)
	{
		const float Now = R.Radiance(T);
		if (!(Now > Prev)) { AddError(FString::Printf(TEXT("not increasing at %.2f K"), T)); break; }
		Prev = Now;
	}
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/BandRadiance.h` not found.

- [ ] **Step 3: Implement** — `Source/CamSimShaders/Public/ThermalFrameParams.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Everything one frame of ThermalCS needs besides its input textures (ROADMAP 4A).
 * This first version holds the in-band radiance LUT only; Task 7 adds the rest.
 */
struct FThermalFrameParams
{
	/** In-band radiance LUT: LogLut[i] = ln B(T_i), T_i = LutMinK + i / LutScale (FBandRadiance::Build). */
	static constexpr int32 LutSize  = 1024;
	static constexpr float LutMinK  = 150.0f;
	static constexpr float LutMaxK  = 1000.0f;   // upper range left for 4C exhausts
	static constexpr float LutScale = static_cast<float>(LutSize - 1) / (LutMaxK - LutMinK);

	float LogLut[LutSize] = {};
};

/**
 * B(T) from the LUT: T clamped to [LutMinK, LutMaxK] (NaN -> LutMinK), ln B interpolated linearly, then exp.
 * HLSL twin: LutRadiance in Shaders/Private/CamSimThermalCommon.ush (same expressions, same order).
 */
inline float ThermalLutRadiance(const float* LogLut, float TK)
{
	float T = TK;
	if (!(T >= FThermalFrameParams::LutMinK)) T = FThermalFrameParams::LutMinK;
	if (T > FThermalFrameParams::LutMaxK) T = FThermalFrameParams::LutMaxK;
	const float X = (T - FThermalFrameParams::LutMinK) * FThermalFrameParams::LutScale;
	const int32 I = FMath::Min(FMath::FloorToInt32(X), FThermalFrameParams::LutSize - 2);
	const float F = X - static_cast<float>(I);
	const float A = LogLut[I];
	return FMath::Exp(A + (LogLut[I + 1] - A) * F);
}
```

`Source/CamSimTest/Thermal/BandRadiance.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ThermalFrameParams.h"

/**
 * In-band blackbody radiance (ROADMAP 4A): Planck's spectral radiance integrated over a detector band
 * [LoUm, HiUm] in W m^-2 sr^-1, and its LUT over FThermalFrameParams::LutMinK..LutMaxK (ln B, interpolated
 * linearly: <= 0.02 % error from 150 K up, CamSim.Thermal.Planck.LutMatchesIntegral). Pure; game thread.
 */
class CAMSIMTEST_API FBandRadiance
{
public:
	/** Simpson steps per LUT entry (smooth integrand over a narrow band: far below the LUT's interpolation error). */
	static constexpr int32 LutSteps = 512;

	/** Planck spectral radiance B_lambda(T), W m^-2 sr^-1 m^-1; 0 for non-positive inputs or an underflowing exponent. */
	static double SpectralRadiance(double LambdaM, double TK);
	/** Integral of B_lambda over [LoUm, HiUm]: Simpson in u = ln(lambda) of B_lambda * lambda (Steps rounded up to even). 0 for an empty band. */
	static double IntegrateBand(double TK, double LoUm, double HiUm, int32 Steps = 4096);

	/** Fill the LUT for the band (1024 * LutSteps evaluations, a few ms, once per band change). */
	void Build(double InLoUm, double InHiUm);
	bool IsBuilt() const { return bBuilt; }
	double GetLoUm() const { return LoUm; }
	double GetHiUm() const { return HiUm; }
	float Radiance(float TK) const { return ThermalLutRadiance(LogLut, TK); }
	const float* GetLogLut() const { return LogLut; }

private:
	float  LogLut[FThermalFrameParams::LutSize] = {};
	double LoUm = 0.0, HiUm = 0.0;
	bool   bBuilt = false;
};
```

`Source/CamSimTest/Thermal/BandRadiance.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/BandRadiance.h"
#include <cmath>

namespace
{
	constexpr double PlanckH = 6.62607015e-34;   // J s
	constexpr double LightC  = 2.99792458e8;     // m s^-1
	constexpr double BoltzK  = 1.380649e-23;     // J K^-1
}

double FBandRadiance::SpectralRadiance(double LambdaM, double TK)
{
	if (!(LambdaM > 0.0) || !(TK > 0.0)) return 0.0;
	const double X = PlanckH * LightC / (LambdaM * BoltzK * TK);
	if (X > 700.0) return 0.0;
	const double L5 = LambdaM * LambdaM * LambdaM * LambdaM * LambdaM;
	return 2.0 * PlanckH * LightC * LightC / (L5 * std::expm1(X));
}

double FBandRadiance::IntegrateBand(double TK, double InLoUm, double InHiUm, int32 Steps)
{
	if (!(InLoUm > 0.0) || !(InHiUm > InLoUm)) return 0.0;
	const int32 N = FMath::Max(2, Steps + (Steps & 1));
	const double U0 = FMath::Loge(InLoUm * 1e-6), U1 = FMath::Loge(InHiUm * 1e-6);
	const double Hs = (U1 - U0) / N;
	auto F = [TK](double U) { const double L = FMath::Exp(U); return SpectralRadiance(L, TK) * L; };
	double Sum = F(U0) + F(U1);
	for (int32 I = 1; I < N; ++I) Sum += ((I & 1) ? 4.0 : 2.0) * F(U0 + I * Hs);
	return Sum * Hs / 3.0;
}

void FBandRadiance::Build(double InLoUm, double InHiUm)
{
	LoUm = InLoUm;
	HiUm = InHiUm;
	const double MinK = FThermalFrameParams::LutMinK, MaxK = FThermalFrameParams::LutMaxK;
	const double Step = (MaxK - MinK) / (FThermalFrameParams::LutSize - 1);
	for (int32 I = 0; I < FThermalFrameParams::LutSize; ++I)
	{
		const double B = IntegrateBand(MinK + I * Step, LoUm, HiUm, LutSteps);
		LogLut[I] = static_cast<float>(FMath::Loge(FMath::Max(B, 1e-300)));   // never ln 0 (NaN in the interpolation)
	}
	bBuilt = true;
}
```

- [ ] **Step 4: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Thermal.Planck` → Expected: `succeeded 4 failed 0`.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalFrameParams.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/BandRadiance.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/BandRadiance.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalPlanckTest.cpp
git commit -F - <<'EOF'
feat(thermal): in-band Planck radiance and its 1024-entry LUT

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 3: Thermal material classes and the `thermal:` config section

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalTypes.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h`, `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp`
- Modify: `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalConfigTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // Thermal/ThermalTypes.h
  enum class EThermalTemperatureSource : uint8 { Model = 0, Water = 1 };
  struct FThermalMaterialSpec
  {
      FString Name;
      TOptional<float> Albedo, Emissivity, ThermalInertia, ConvectionWm2K, KFast;
      FString Temperature;   // "" keeps the class's source; "model" | "water"
      bool operator==(const FThermalMaterialSpec&) const = default;
  };
  // Thermal/ThermalMaterials.h
  struct FThermalMaterial { FString Name; float Albedo, Emissivity, ThermalInertia, ConvectionWm2K, KFast; EThermalTemperatureSource Source; };
  class CAMSIMTEST_API FThermalMaterialTable
  {
  public:
      static constexpr int32 MaxClasses = 32;
      static constexpr int32 TerrainDefault = 0, Water = 1, VehiclePaint = 2;
      static const TArray<FThermalMaterial>& BuiltIns();
      static bool ParseSource(const FString& Name, EThermalTemperatureSource& Out);
      static TArray<FString> Validate(const TArray<FThermalMaterialSpec>& Specs);
      FThermalMaterialTable();                                          // built-ins only
      TArray<FString> Build(const TArray<FThermalMaterialSpec>& Specs); // invalid specs skipped, their errors returned
      int32 Find(const FString& Name) const;                            // INDEX_NONE if absent
      int32 Num() const;
      const FThermalMaterial& Get(int32 Index) const;
      uint32 GetVersion() const;                                        // changes on every Build
  };
  // Config/CamSimConfig.h, inside FCamSimConfig
  struct FThermalConfig
  {
      bool  bEnabled = true;
      float AirTemperatureC = 15.0f;
      float AirDiurnalSwingK = 8.0f;
      float ExtinctionPerKmMwir = 0.15f;
      float ExtinctionPerKmLwir = 0.10f;
      float FogIrFactor = 0.4f;
      TArray<FThermalMaterialSpec> Materials;
  };
  FThermalConfig Thermal;
  ```
  Env: `CAMSIM_THERMAL_ENABLED`, `CAMSIM_THERMAL_AIR_TEMPERATURE_C`, `CAMSIM_THERMAL_AIR_DIURNAL_SWING_K`, `CAMSIM_THERMAL_EXTINCTION_MWIR`, `CAMSIM_THERMAL_EXTINCTION_LWIR`, `CAMSIM_THERMAL_FOG_IR_FACTOR`.

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Config/CamSimConfig.h"
#include "Thermal/ThermalMaterials.h"

#include <limits>

// CamSim.Thermal.Config.*: the thermal: section and the material table (ROADMAP 4A).

namespace
{
	bool HasError(const TArray<FString>& Errors, const TCHAR* Needle)
	{
		return Errors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalConfigDefaultsTest, "CamSim.Thermal.Config.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalConfigDefaultsTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig::FThermalConfig T;
	TestTrue (TEXT("enabled"), T.bEnabled);
	TestEqual(TEXT("air temperature"), T.AirTemperatureC, 15.0f);
	TestEqual(TEXT("diurnal swing"), T.AirDiurnalSwingK, 8.0f);
	TestEqual(TEXT("MWIR extinction"), T.ExtinctionPerKmMwir, 0.15f);
	TestEqual(TEXT("LWIR extinction"), T.ExtinctionPerKmLwir, 0.10f);
	TestEqual(TEXT("fog factor"), T.FogIrFactor, 0.4f);
	TestEqual(TEXT("no material overrides"), T.Materials.Num(), 0);
	TestFalse(TEXT("defaults valid"), HasError(FCamSimConfig().Validate(), TEXT("thermal")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalConfigYamlTest, "CamSim.Thermal.Config.YamlAndEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalConfigYamlTest::RunTest(const FString& Parameters)
{
	const FString Yaml = TEXT(
		"thermal:\n"
		"  enabled: false\n"
		"  air_temperature_c: 22.5\n"
		"  air_diurnal_swing_k: 12\n"
		"  extinction_per_km:\n"
		"    mwir: 0.3\n"
		"    lwir: 0.2\n"
		"  fog_ir_factor: 0.6\n"
		"  materials:\n"
		"    asphalt:\n"
		"      albedo: 0.08\n"
		"      k_fast: 0.025\n"
		"    gravel:\n"
		"      emissivity: 0.93\n"
		"      thermal_inertia: 1000\n"
		"      convection_w_m2k: 11\n"
		"      temperature: model\n");
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	TestTrue (TEXT("parsed"), Cfg.bLoadedSuccessfully);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestFalse(TEXT("enabled"), Cfg.Thermal.bEnabled);
	TestEqual(TEXT("air"), Cfg.Thermal.AirTemperatureC, 22.5f);
	TestEqual(TEXT("swing"), Cfg.Thermal.AirDiurnalSwingK, 12.0f);
	TestEqual(TEXT("mwir"), Cfg.Thermal.ExtinctionPerKmMwir, 0.3f);
	TestEqual(TEXT("lwir"), Cfg.Thermal.ExtinctionPerKmLwir, 0.2f);
	TestEqual(TEXT("fog"), Cfg.Thermal.FogIrFactor, 0.6f);
	if (TestEqual(TEXT("two material specs"), Cfg.Thermal.Materials.Num(), 2))
	{
		const FThermalMaterialSpec& A = Cfg.Thermal.Materials[0];
		TestEqual(TEXT("asphalt name"), A.Name, FString(TEXT("asphalt")));
		TestTrue (TEXT("asphalt albedo set"), A.Albedo.IsSet() && *A.Albedo == 0.08f);
		TestFalse(TEXT("asphalt emissivity not set"), A.Emissivity.IsSet());
		const FThermalMaterialSpec& G = Cfg.Thermal.Materials[1];
		TestEqual(TEXT("gravel temperature"), G.Temperature, FString(TEXT("model")));
		TestTrue (TEXT("gravel inertia"), G.ThermalInertia.IsSet() && *G.ThermalInertia == 1000.0f);
	}
	TestTrue(TEXT("a typo is an unknown key"),
		FCamSimConfig::LoadFromYamlString(TEXT("thermal:\n  materials:\n    asphalt:\n      albedoo: 0.1\n")).UnknownYamlKeys
			.Contains(TEXT("thermal.materials.asphalt.albedoo")));

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_ENABLED"), TEXT("true"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_AIR_TEMPERATURE_C"), TEXT("5"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_AIR_DIURNAL_SWING_K"), TEXT("4"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_EXTINCTION_MWIR"), TEXT("0.5"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_EXTINCTION_LWIR"), TEXT("0.25"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_THERMAL_FOG_IR_FACTOR"), TEXT("1.5"));
	Cfg = FCamSimConfig::LoadFromYamlString(Yaml);
	for (const TCHAR* K : { TEXT("CAMSIM_THERMAL_ENABLED"), TEXT("CAMSIM_THERMAL_AIR_TEMPERATURE_C"), TEXT("CAMSIM_THERMAL_AIR_DIURNAL_SWING_K"),
		TEXT("CAMSIM_THERMAL_EXTINCTION_MWIR"), TEXT("CAMSIM_THERMAL_EXTINCTION_LWIR"), TEXT("CAMSIM_THERMAL_FOG_IR_FACTOR") })
	{
		FPlatformMisc::SetEnvironmentVar(K, TEXT(""));
	}
	TestTrue (TEXT("env enabled"), Cfg.Thermal.bEnabled);
	TestEqual(TEXT("env air"), Cfg.Thermal.AirTemperatureC, 5.0f);
	TestEqual(TEXT("env swing"), Cfg.Thermal.AirDiurnalSwingK, 4.0f);
	TestEqual(TEXT("env mwir"), Cfg.Thermal.ExtinctionPerKmMwir, 0.5f);
	TestEqual(TEXT("env lwir"), Cfg.Thermal.ExtinctionPerKmLwir, 0.25f);
	TestEqual(TEXT("env fog"), Cfg.Thermal.FogIrFactor, 1.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalConfigValidateTest, "CamSim.Thermal.Config.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalConfigValidateTest::RunTest(const FString& Parameters)
{
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	auto With = [](TFunction<void(FCamSimConfig::FThermalConfig&)> Set) { FCamSimConfig C; Set(C.Thermal); return C.Validate(); };
	TestTrue (TEXT("air -100"),  HasError(With([](auto& T) { T.AirTemperatureC = -100.0f; }), TEXT("thermal.air_temperature_c")));
	TestTrue (TEXT("air NaN"),   HasError(With([NaN](auto& T) { T.AirTemperatureC = NaN; }), TEXT("thermal.air_temperature_c")));
	TestTrue (TEXT("swing 40"),  HasError(With([](auto& T) { T.AirDiurnalSwingK = 40.0f; }), TEXT("thermal.air_diurnal_swing_k")));
	TestTrue (TEXT("mwir < 0"),  HasError(With([](auto& T) { T.ExtinctionPerKmMwir = -0.1f; }), TEXT("thermal.extinction_per_km.mwir")));
	TestTrue (TEXT("lwir NaN"),  HasError(With([NaN](auto& T) { T.ExtinctionPerKmLwir = NaN; }), TEXT("thermal.extinction_per_km.lwir")));
	TestTrue (TEXT("fog 3"),     HasError(With([](auto& T) { T.FogIrFactor = 3.0f; }), TEXT("thermal.fog_ir_factor")));
	auto WithSpec = [&](TFunction<void(FThermalMaterialSpec&)> Set)
	{
		return With([Set](FCamSimConfig::FThermalConfig& T) { FThermalMaterialSpec S; S.Name = TEXT("asphalt"); Set(S); T.Materials.Add(S); });
	};
	TestTrue (TEXT("albedo 1"),        HasError(WithSpec([](auto& S) { S.Albedo = 1.0f; }), TEXT("thermal.materials.asphalt.albedo")));
	TestTrue (TEXT("emissivity 0"),    HasError(WithSpec([](auto& S) { S.Emissivity = 0.0f; }), TEXT("thermal.materials.asphalt.emissivity")));
	TestTrue (TEXT("inertia -1"),      HasError(WithSpec([](auto& S) { S.ThermalInertia = -1.0f; }), TEXT("thermal.materials.asphalt.thermal_inertia")));
	TestTrue (TEXT("convection 0"),    HasError(WithSpec([](auto& S) { S.ConvectionWm2K = 0.0f; }), TEXT("thermal.materials.asphalt.convection_w_m2k")));
	TestTrue (TEXT("k_fast 0.5"),      HasError(WithSpec([](auto& S) { S.KFast = 0.5f; }), TEXT("thermal.materials.asphalt.k_fast")));
	TestTrue (TEXT("temperature lava"), HasError(WithSpec([](auto& S) { S.Temperature = TEXT("lava"); }), TEXT("thermal.materials.asphalt.temperature")));
	TestTrue (TEXT("bad name"),        HasError(With([](auto& T) { FThermalMaterialSpec S; S.Name = TEXT("Bad Name"); T.Materials.Add(S); }), TEXT("thermal.materials")));
	TestFalse(TEXT("valid spec"),      HasError(WithSpec([](auto& S) { S.Albedo = 0.1f; S.Temperature = TEXT("water"); }), TEXT("thermal")));
	TestTrue (TEXT("too many classes"), HasError(With([](FCamSimConfig::FThermalConfig& T)
	{
		for (int32 I = 0; I < FThermalMaterialTable::MaxClasses; ++I) { FThermalMaterialSpec S; S.Name = FString::Printf(TEXT("class_%d"), I); T.Materials.Add(S); }
	}), TEXT("thermal.materials")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalMaterialTableTest, "CamSim.Thermal.Config.MaterialTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalMaterialTableTest::RunTest(const FString& Parameters)
{
	FThermalMaterialTable T;
	TestEqual(TEXT("terrain_default index"), T.Find(TEXT("terrain_default")), FThermalMaterialTable::TerrainDefault);
	TestEqual(TEXT("water index"), T.Find(TEXT("water")), FThermalMaterialTable::Water);
	TestEqual(TEXT("vehicle_paint index"), T.Find(TEXT("vehicle_paint")), FThermalMaterialTable::VehiclePaint);
	TestTrue (TEXT("water is fixed"), T.Get(FThermalMaterialTable::Water).Source == EThermalTemperatureSource::Water);
	TestEqual(TEXT("asphalt k_fast (spec)"), T.Get(T.Find(TEXT("asphalt"))).KFast, 0.02f);
	TestEqual(TEXT("vegetation k_fast (spec)"), T.Get(T.Find(TEXT("vegetation"))).KFast, 0.008f);
	TestEqual(TEXT("vehicle_paint k_fast (spec)"), T.Get(FThermalMaterialTable::VehiclePaint).KFast, 0.04f);
	TestEqual(TEXT("unknown"), T.Find(TEXT("unobtainium")), static_cast<int32>(INDEX_NONE));

	const uint32 V0 = T.GetVersion();
	TArray<FThermalMaterialSpec> Specs;
	{ FThermalMaterialSpec S; S.Name = TEXT("asphalt"); S.Albedo = 0.05f; Specs.Add(S); }
	{ FThermalMaterialSpec S; S.Name = TEXT("gravel"); S.Emissivity = 0.93f; Specs.Add(S); }
	{ FThermalMaterialSpec S; S.Name = TEXT("broken"); S.Albedo = 2.0f; Specs.Add(S); }
	const TArray<FString> Errors = T.Build(Specs);
	TestTrue (TEXT("version changed"), T.GetVersion() != V0);
	TestEqual(TEXT("one error (the broken spec)"), Errors.Num(), 1);
	TestEqual(TEXT("broken skipped"), T.Find(TEXT("broken")), static_cast<int32>(INDEX_NONE));
	const FThermalMaterial& A = T.Get(T.Find(TEXT("asphalt")));
	TestEqual(TEXT("asphalt albedo overridden"), A.Albedo, 0.05f);
	TestEqual(TEXT("asphalt emissivity kept"), A.Emissivity, 0.95f);
	const int32 Gi = T.Find(TEXT("gravel"));
	if (TestTrue(TEXT("gravel added"), Gi >= FThermalMaterialTable::BuiltIns().Num()))
	{
		const FThermalMaterial& G = T.Get(Gi);
		TestEqual(TEXT("gravel emissivity"), G.Emissivity, 0.93f);
		TestEqual(TEXT("gravel inherits terrain_default inertia"), G.ThermalInertia, T.Get(FThermalMaterialTable::TerrainDefault).ThermalInertia);
	}
	TestEqual(TEXT("built-in indices unchanged"), T.Find(TEXT("water")), FThermalMaterialTable::Water);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalConfigCanonicalTest, "CamSim.Thermal.Config.CanonicalConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalConfigCanonicalTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../../deploy/camsim_config.yaml")));
	FString Yaml;
	if (!TestTrue(FString::Printf(TEXT("read %s"), *Path), FFileHelper::LoadFileToString(Yaml, *Path))) return false;
	TestTrue(TEXT("canonical yaml has a thermal: section"), Yaml.Contains(TEXT("\nthermal:")));
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml, Path);
	const FCamSimConfig::FThermalConfig D;
	TestEqual(TEXT("enabled = default"), Cfg.Thermal.bEnabled, D.bEnabled);
	TestEqual(TEXT("air = default"), Cfg.Thermal.AirTemperatureC, D.AirTemperatureC);
	TestEqual(TEXT("swing = default"), Cfg.Thermal.AirDiurnalSwingK, D.AirDiurnalSwingK);
	TestEqual(TEXT("mwir = default"), Cfg.Thermal.ExtinctionPerKmMwir, D.ExtinctionPerKmMwir);
	TestEqual(TEXT("lwir = default"), Cfg.Thermal.ExtinctionPerKmLwir, D.ExtinctionPerKmLwir);
	TestEqual(TEXT("fog = default"), Cfg.Thermal.FogIrFactor, D.FogIrFactor);
	TestFalse(TEXT("no thermal validation errors"), HasError(Cfg.Validate(), TEXT("thermal")));
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/ThermalMaterials.h` not found.

- [ ] **Step 3: Implement the types and the table.** `Source/CamSimTest/Thermal/ThermalTypes.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Where a thermal class's temperature comes from (ROADMAP 4A). */
enum class EThermalTemperatureSource : uint8
{
	Model = 0,   // closed-form diurnal response (FThermalModel)
	Water = 1,   // the water temperature (CIGI Maritime Surface / ocean.water_temperature_c) +- 0.5 K diurnal
};

/** thermal.materials.<name>: overrides of one built-in class, or a new class (unset fields copy terrain_default). */
struct FThermalMaterialSpec
{
	FString Name;
	TOptional<float> Albedo;           // [0, 1)
	TOptional<float> Emissivity;       // (0, 1]
	TOptional<float> ThermalInertia;   // J m^-2 K^-1 s^-1/2, [0, 20000]
	TOptional<float> ConvectionWm2K;   // W m^-2 K^-1, (0, 200]
	TOptional<float> KFast;            // K per W m^-2, [0, 0.2]
	FString Temperature;               // "" (keep), "model", "water"

	bool operator==(const FThermalMaterialSpec&) const = default;
};
```

`Source/CamSimTest/Thermal/ThermalMaterials.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Thermal/ThermalTypes.h"

/** One thermal class (ROADMAP 4A): optical and thermal properties for FThermalModel and ThermalCS. */
struct FThermalMaterial
{
	FString Name;
	float Albedo         = 0.20f;    // solar
	float Emissivity     = 0.95f;    // in band and broadband
	float ThermalInertia = 1200.0f;  // J m^-2 K^-1 s^-1/2
	float ConvectionWm2K = 10.0f;    // h_c; h = h_c + 4 eps sigma T_air^3
	float KFast          = 0.015f;   // K per W m^-2 of absorbed flux above the class reference (per-pixel fast term)
	EThermalTemperatureSource Source = EThermalTemperatureSource::Model;
};

/**
 * Built-in classes plus thermal.materials overrides, name -> index (<= MaxClasses). The first three indices
 * are fixed: terrain_default (every terrain pixel in 4A), water, vehicle_paint (entities by default).
 * Game thread.
 */
class CAMSIMTEST_API FThermalMaterialTable
{
public:
	static constexpr int32 MaxClasses     = 32;   // == FThermalFrameParams::MaxClasses (static_assert in ThermalFrameBuilder.cpp)
	static constexpr int32 TerrainDefault = 0;
	static constexpr int32 Water          = 1;
	static constexpr int32 VehiclePaint   = 2;

	static const TArray<FThermalMaterial>& BuiltIns();
	/** "model" / "water" (case-insensitive). */
	static bool ParseSource(const FString& Name, EThermalTemperatureSource& Out);
	/** Errors for thermal.materials as FCamSimConfig::Validate reports them (names, ranges, sources, class count). */
	static TArray<FString> Validate(const TArray<FThermalMaterialSpec>& Specs);

	FThermalMaterialTable();
	/** Built-ins, then each valid spec applied in order (a new name starts as a copy of terrain_default). Returns the skipped specs' errors. */
	TArray<FString> Build(const TArray<FThermalMaterialSpec>& Specs);
	int32 Find(const FString& Name) const;
	int32 Num() const { return Classes.Num(); }
	const FThermalMaterial& Get(int32 Index) const { return Classes[Index]; }
	uint32 GetVersion() const { return Version; }

private:
	TArray<FThermalMaterial> Classes;
	uint32 Version = 0;
};
```

`Source/CamSimTest/Thermal/ThermalMaterials.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalMaterials.h"

namespace
{
	uint32 GNextMaterialsVersion = 1;   // game thread

	FThermalMaterial MakeMaterial(const TCHAR* Name, float Albedo, float Emissivity, float Inertia, float Convection, float KFast,
		EThermalTemperatureSource Source = EThermalTemperatureSource::Model)
	{
		FThermalMaterial M;
		M.Name = Name; M.Albedo = Albedo; M.Emissivity = Emissivity; M.ThermalInertia = Inertia;
		M.ConvectionWm2K = Convection; M.KFast = KFast; M.Source = Source;
		return M;
	}

	bool IsValidName(const FString& N)
	{
		if (N.IsEmpty()) return false;
		for (const TCHAR C : N)
		{
			if (!((C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_'))) return false;
		}
		return true;
	}

	/** Range errors of one spec. Written !(x in range) so NaN is reported too. */
	TArray<FString> ValidateOne(const FThermalMaterialSpec& S)
	{
		TArray<FString> E;
		const FString P = FString::Printf(TEXT("thermal.materials.%s"), *S.Name);
		if (!IsValidName(S.Name))
			E.Add(FString::Printf(TEXT("thermal.materials: name '%s' must be lower-case letters, digits or '_'"), *S.Name));
		if (S.Albedo.IsSet() && !(*S.Albedo >= 0.0f && *S.Albedo < 1.0f))
			E.Add(FString::Printf(TEXT("%s.albedo=%.3f out of range [0, 1)"), *P, *S.Albedo));
		if (S.Emissivity.IsSet() && !(*S.Emissivity > 0.0f && *S.Emissivity <= 1.0f))
			E.Add(FString::Printf(TEXT("%s.emissivity=%.3f out of range (0, 1]"), *P, *S.Emissivity));
		if (S.ThermalInertia.IsSet() && !(*S.ThermalInertia >= 0.0f && *S.ThermalInertia <= 20000.0f))
			E.Add(FString::Printf(TEXT("%s.thermal_inertia=%.1f out of range [0, 20000]"), *P, *S.ThermalInertia));
		if (S.ConvectionWm2K.IsSet() && !(*S.ConvectionWm2K > 0.0f && *S.ConvectionWm2K <= 200.0f))
			E.Add(FString::Printf(TEXT("%s.convection_w_m2k=%.2f out of range (0, 200]"), *P, *S.ConvectionWm2K));
		if (S.KFast.IsSet() && !(*S.KFast >= 0.0f && *S.KFast <= 0.2f))
			E.Add(FString::Printf(TEXT("%s.k_fast=%.4f out of range [0, 0.2]"), *P, *S.KFast));
		EThermalTemperatureSource Unused;
		if (!S.Temperature.IsEmpty() && !FThermalMaterialTable::ParseSource(S.Temperature, Unused))
			E.Add(FString::Printf(TEXT("%s.temperature '%s' must be model or water"), *P, *S.Temperature));
		return E;
	}

	bool IsBuiltIn(const FString& Name)
	{
		return FThermalMaterialTable::BuiltIns().ContainsByPredicate([&Name](const FThermalMaterial& M) { return M.Name == Name; });
	}
}

const TArray<FThermalMaterial>& FThermalMaterialTable::BuiltIns()
{
	// k_fast per the spec (asphalt 0.02, vegetation 0.008, metal paint 0.04). Index order is fixed (see the header).
	static const TArray<FThermalMaterial> B = {
		MakeMaterial(TEXT("terrain_default"), 0.20f, 0.95f, 1200.0f, 10.0f, 0.015f),
		MakeMaterial(TEXT("water"),           0.06f, 0.98f,    0.0f, 10.0f, 0.0f, EThermalTemperatureSource::Water),
		MakeMaterial(TEXT("vehicle_paint"),   0.30f, 0.90f,  600.0f, 12.0f, 0.04f),
		MakeMaterial(TEXT("asphalt"),         0.10f, 0.95f, 1500.0f, 10.0f, 0.02f),
		MakeMaterial(TEXT("vegetation"),      0.20f, 0.98f,  300.0f, 15.0f, 0.008f),
		MakeMaterial(TEXT("concrete"),        0.35f, 0.92f, 1800.0f, 10.0f, 0.015f),
	};
	return B;
}

bool FThermalMaterialTable::ParseSource(const FString& Name, EThermalTemperatureSource& Out)
{
	if (Name.Equals(TEXT("model"), ESearchCase::IgnoreCase)) { Out = EThermalTemperatureSource::Model; return true; }
	if (Name.Equals(TEXT("water"), ESearchCase::IgnoreCase)) { Out = EThermalTemperatureSource::Water; return true; }
	return false;
}

TArray<FString> FThermalMaterialTable::Validate(const TArray<FThermalMaterialSpec>& Specs)
{
	TArray<FString> Errors;
	TSet<FString> Seen;
	int32 NewNames = 0;
	for (const FThermalMaterialSpec& S : Specs)
	{
		Errors.Append(ValidateOne(S));
		if (Seen.Contains(S.Name))
		{
			Errors.Add(FString::Printf(TEXT("thermal.materials.%s is listed twice"), *S.Name));
		}
		else
		{
			Seen.Add(S.Name);
			NewNames += IsBuiltIn(S.Name) ? 0 : 1;
		}
	}
	if (BuiltIns().Num() + NewNames > MaxClasses)
	{
		Errors.Add(FString::Printf(TEXT("thermal.materials: %d built-in + %d new classes exceed %d"), BuiltIns().Num(), NewNames, MaxClasses));
	}
	return Errors;
}

FThermalMaterialTable::FThermalMaterialTable()
	: Classes(BuiltIns())
	, Version(GNextMaterialsVersion++)
{
}

TArray<FString> FThermalMaterialTable::Build(const TArray<FThermalMaterialSpec>& Specs)
{
	TArray<FString> Errors;
	Classes = BuiltIns();
	for (const FThermalMaterialSpec& S : Specs)
	{
		const TArray<FString> E = ValidateOne(S);
		if (E.Num() > 0)
		{
			Errors.Append(E);
			continue;
		}
		int32 Index = Find(S.Name);
		if (Index == INDEX_NONE)
		{
			if (Classes.Num() >= MaxClasses)
			{
				Errors.Add(FString::Printf(TEXT("thermal.materials.%s skipped: more than %d classes"), *S.Name, MaxClasses));
				continue;
			}
			FThermalMaterial M = Classes[TerrainDefault];
			M.Name = S.Name;
			Index = Classes.Add(M);
		}
		FThermalMaterial& M = Classes[Index];
		if (S.Albedo.IsSet())         M.Albedo = *S.Albedo;
		if (S.Emissivity.IsSet())     M.Emissivity = *S.Emissivity;
		if (S.ThermalInertia.IsSet()) M.ThermalInertia = *S.ThermalInertia;
		if (S.ConvectionWm2K.IsSet()) M.ConvectionWm2K = *S.ConvectionWm2K;
		if (S.KFast.IsSet())          M.KFast = *S.KFast;
		if (!S.Temperature.IsEmpty()) ParseSource(S.Temperature, M.Source);
	}
	Version = GNextMaterialsVersion++;
	return Errors;
}

int32 FThermalMaterialTable::Find(const FString& Name) const
{
	for (int32 I = 0; I < Classes.Num(); ++I)
	{
		if (Classes[I].Name == Name) return I;
	}
	return INDEX_NONE;
}
```

- [ ] **Step 4: Implement the config section.** In `Config/CamSimConfig.h` add `#include "Thermal/ThermalTypes.h"` after the `Sensor/SensorTypes.h` include, and after `FOceanConfig Ocean;`:

```cpp
	/** Thermal radiance for IR (ROADMAP 4A, docs/thermal.md). */
	struct FThermalConfig
	{
		// Startup decides whether thermal is available (and entities get stencils for it); a live
		// false (hot reload) runs IR as the 3B.2 luminance proxy for A/B comparison.
		bool  bEnabled            = true;
		float AirTemperatureC     = 15.0f;   // daily mean, until CIGI Atmosphere Control sets it
		float AirDiurnalSwingK    = 8.0f;    // peak-to-peak; T_air = mean + swing/2 cos(w (t - 15 h local solar))
		float ExtinctionPerKmMwir = 0.15f;   // band extinction, per km
		float ExtinctionPerKmLwir = 0.10f;
		float FogIrFactor         = 0.4f;    // beta_fog = 3.912 / V_km * factor (IR sees farther than visible)
		TArray<FThermalMaterialSpec> Materials;   // thermal.materials overrides / additions (yaml only)
	};
	FThermalConfig Thermal;
```

In `Config/CamSimConfig.cpp` add `#include "Thermal/ThermalMaterials.h"` after `#include "Sensor/SensorPresets.h"`; after the `if (YamlHas(Root, "ocean")) { … }` block in `LoadFromYaml`:

```cpp
		// Thermal radiance for IR (ROADMAP 4A)
		if (YamlHas(Root, "thermal"))
		{
			ryml::ConstNodeRef T = Root["thermal"];
			YamlBool (T, "enabled",             Cfg.Thermal.bEnabled);
			YamlFloat(T, "air_temperature_c",   Cfg.Thermal.AirTemperatureC);
			YamlFloat(T, "air_diurnal_swing_k", Cfg.Thermal.AirDiurnalSwingK);
			if (YamlHas(T, "extinction_per_km"))
			{
				ryml::ConstNodeRef X = T["extinction_per_km"];
				YamlFloat(X, "mwir", Cfg.Thermal.ExtinctionPerKmMwir);
				YamlFloat(X, "lwir", Cfg.Thermal.ExtinctionPerKmLwir);
			}
			YamlFloat(T, "fog_ir_factor",       Cfg.Thermal.FogIrFactor);
			if (YamlHas(T, "materials"))
			{
				ryml::ConstNodeRef Ms = T["materials"];
				if (Ms.is_map())
				{
					YamlKeysAreData(Ms);   // class names; their fields are still checked
					for (ryml::ConstNodeRef MNode : Ms)
					{
						FThermalMaterialSpec Spec;
						Spec.Name = RymlToFString(MNode.key());
						if (MNode.is_map())
						{
							float V = 0.0f;
							if (YamlFloat(MNode, "albedo", V))           Spec.Albedo = V;
							if (YamlFloat(MNode, "emissivity", V))       Spec.Emissivity = V;
							if (YamlFloat(MNode, "thermal_inertia", V))  Spec.ThermalInertia = V;
							if (YamlFloat(MNode, "convection_w_m2k", V)) Spec.ConvectionWm2K = V;
							if (YamlFloat(MNode, "k_fast", V))           Spec.KFast = V;
							YamlString(MNode, "temperature", Spec.Temperature);
						}
						Cfg.Thermal.Materials.Add(MoveTemp(Spec));
					}
				}
			}
		}
```

In `ApplyEnvOverrides`, after the `// Ocean (ROADMAP 2.6)` block:

```cpp
	// Thermal (ROADMAP 4A)
	Cfg.Thermal.bEnabled            = GetEnvBool (TEXT("CAMSIM_THERMAL_ENABLED"),             Cfg.Thermal.bEnabled);
	Cfg.Thermal.AirTemperatureC     = GetEnvFloat(TEXT("CAMSIM_THERMAL_AIR_TEMPERATURE_C"),   Cfg.Thermal.AirTemperatureC);
	Cfg.Thermal.AirDiurnalSwingK    = GetEnvFloat(TEXT("CAMSIM_THERMAL_AIR_DIURNAL_SWING_K"), Cfg.Thermal.AirDiurnalSwingK);
	Cfg.Thermal.ExtinctionPerKmMwir = GetEnvFloat(TEXT("CAMSIM_THERMAL_EXTINCTION_MWIR"),     Cfg.Thermal.ExtinctionPerKmMwir);
	Cfg.Thermal.ExtinctionPerKmLwir = GetEnvFloat(TEXT("CAMSIM_THERMAL_EXTINCTION_LWIR"),     Cfg.Thermal.ExtinctionPerKmLwir);
	Cfg.Thermal.FogIrFactor         = GetEnvFloat(TEXT("CAMSIM_THERMAL_FOG_IR_FACTOR"),       Cfg.Thermal.FogIrFactor);
```

In `Validate()`, after the `// Ocean (ROADMAP 2.6)` checks:

```cpp
	// Thermal (ROADMAP 4A). Written !(x in range) so NaN is reported too.
	if (!(Thermal.AirTemperatureC >= -80.0f && Thermal.AirTemperatureC <= 60.0f))
		Errors.Add(FString::Printf(TEXT("thermal.air_temperature_c=%.2f out of range [-80, 60]"), Thermal.AirTemperatureC));
	if (!(Thermal.AirDiurnalSwingK >= 0.0f && Thermal.AirDiurnalSwingK <= 30.0f))
		Errors.Add(FString::Printf(TEXT("thermal.air_diurnal_swing_k=%.2f out of range [0, 30]"), Thermal.AirDiurnalSwingK));
	if (!(Thermal.ExtinctionPerKmMwir >= 0.0f && Thermal.ExtinctionPerKmMwir <= 10.0f))
		Errors.Add(FString::Printf(TEXT("thermal.extinction_per_km.mwir=%.3f out of range [0, 10]"), Thermal.ExtinctionPerKmMwir));
	if (!(Thermal.ExtinctionPerKmLwir >= 0.0f && Thermal.ExtinctionPerKmLwir <= 10.0f))
		Errors.Add(FString::Printf(TEXT("thermal.extinction_per_km.lwir=%.3f out of range [0, 10]"), Thermal.ExtinctionPerKmLwir));
	if (!(Thermal.FogIrFactor >= 0.0f && Thermal.FogIrFactor <= 2.0f))
		Errors.Add(FString::Printf(TEXT("thermal.fog_ir_factor=%.2f out of range [0, 2]"), Thermal.FogIrFactor));
	Errors.Append(FThermalMaterialTable::Validate(Thermal.Materials));
```

- [ ] **Step 5: Canonical yaml and docs.** In `deploy/camsim_config.yaml`, after the `ocean:` block (before `# Phase 27 — Performance & Optimization`):

```yaml
# --- Thermal radiance for IR (ROADMAP 4A, docs/thermal.md) ---
# IR mode renders in-band (MWIR/LWIR) radiance from per-pixel surface temperature and emissivity.
# enabled: false restores the 3B.2 luminance proxy (startup decides availability; a hot reload to
# false switches IR back to the proxy for A/B comparison).
thermal:
  enabled: true                 # CAMSIM_THERMAL_ENABLED
  air_temperature_c: 15.0       # CAMSIM_THERMAL_AIR_TEMPERATURE_C — daily mean until CIGI Atmosphere Control sets it
  air_diurnal_swing_k: 8.0      # CAMSIM_THERMAL_AIR_DIURNAL_SWING_K — peak-to-peak, peak at 15:00 local solar
  extinction_per_km:
    mwir: 0.15                  # CAMSIM_THERMAL_EXTINCTION_MWIR
    lwir: 0.10                  # CAMSIM_THERMAL_EXTINCTION_LWIR
  fog_ir_factor: 0.4            # CAMSIM_THERMAL_FOG_IR_FACTOR — beta_fog = 3.912 / V_km * factor
  # materials:                  # overrides of / additions to the built-in classes (yaml only)
  #   asphalt:
  #     albedo: 0.1
  #     emissivity: 0.95
  #     thermal_inertia: 1500   # J m^-2 K^-1 s^-1/2
  #     convection_w_m2k: 10
  #     k_fast: 0.02            # K per W m^-2 absorbed above the class reference
  #     temperature: model      # model | water
```

In `docs/configuration.md`, after the `## Ocean (`ocean:`)` section (end of file), append:

````markdown
## Thermal (`thermal:`)

IR radiance (ROADMAP 4A, guide: [`docs/thermal.md`](thermal.md)). In IR mode each pixel's
in-band radiance comes from a surface temperature (a closed-form diurnal model per material
class, plus a per-pixel solar term from the EO render) and emissivity, with sky and path terms,
and goes through the unchanged sensor model. The band comes from the IR preset
(`detector.band_lo_um`/`band_hi_um`).

```yaml
thermal:
  enabled: true
  air_temperature_c: 15.0
  air_diurnal_swing_k: 8.0
  extinction_per_km: {mwir: 0.15, lwir: 0.10}
  fog_ir_factor: 0.4
  materials:            # optional
    asphalt: {albedo: 0.1, k_fast: 0.02}
```

| Key | Env | Default | Description |
|---|---|---|---|
| `thermal.enabled` | `CAMSIM_THERMAL_ENABLED` | `true` | **Startup** decides whether the thermal pass is available (and entities get custom-depth stencils for it). A hot reload to `false` runs IR as the 3B.2 luminance proxy (A/B); back to `true` restores thermal if it was available at startup. |
| `thermal.air_temperature_c` | `CAMSIM_THERMAL_AIR_TEMPERATURE_C` | `15.0` | Daily mean air temperature until a CIGI Atmosphere Control packet sets one; `[-80, 60]`. |
| `thermal.air_diurnal_swing_k` | `CAMSIM_THERMAL_AIR_DIURNAL_SWING_K` | `8.0` | Peak-to-peak diurnal air swing, peak at 15:00 local solar; `[0, 30]`. |
| `thermal.extinction_per_km.mwir` / `.lwir` | `CAMSIM_THERMAL_EXTINCTION_MWIR` / `_LWIR` | `0.15` / `0.10` | Band extinction β per km; the band is MWIR when the IR preset's band centre is below 6.5 µm. `[0, 10]`. |
| `thermal.fog_ir_factor` | `CAMSIM_THERMAL_FOG_IR_FACTOR` | `0.4` | While CIGI Atmosphere Control has fog enabled, β += 3.912 / V_km × factor; `[0, 2]`. |
| `thermal.materials.<name>` | *(yaml only)* | — | Overrides a built-in class (`terrain_default`, `water`, `vehicle_paint`, `asphalt`, `vegetation`, `concrete`) or adds one (unset fields copy `terrain_default`; ≤ 32 classes). Fields: `albedo` `[0,1)`, `emissivity` `(0,1]`, `thermal_inertia` J m⁻² K⁻¹ s⁻½ `[0,20000]`, `convection_w_m2k` `(0,200]`, `k_fast` K/(W m⁻²) `[0,0.2]`, `temperature` `model`\|`water`. Names: lower-case letters, digits, `_`. |

Built-in classes:

| Class | albedo | ε | inertia | h_c | k_fast | temperature |
|---|---|---|---|---|---|---|
| `terrain_default` | 0.20 | 0.95 | 1200 | 10 | 0.015 | model |
| `water` | 0.06 | 0.98 | — | — | 0 | water ± 0.5 K |
| `vehicle_paint` | 0.30 | 0.90 | 600 | 12 | 0.04 | model |
| `asphalt` | 0.10 | 0.95 | 1500 | 10 | 0.02 | model |
| `vegetation` | 0.20 | 0.98 | 300 | 15 | 0.008 | model |
| `concrete` | 0.35 | 0.92 | 1800 | 10 | 0.015 | model |
````

- [ ] **Step 6: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Thermal.Config && run_tests CamSim.Config` → Expected: `CamSim.Thermal.Config` 5 succeeded 0 failed; `CamSim.Config` 0 failed (`CanonicalConfigHasNoUnknownKeys` included).

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalTypes.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalMaterials.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalConfigTest.cpp \
  deploy/camsim_config.yaml docs/configuration.md
git commit -F - <<'EOF'
feat(thermal): material classes and the thermal: config section

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 4: Sensor, ocean and entity-type keys the thermal path reads

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorTypes.h`, `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPresets.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h`, `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Entity/EntityTypeTable.h`, `unreal_project/CamSimTest/Source/CamSimTest/Entity/EntityTypeTable.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` (initial water temperature)
- Modify: `deploy/camsim_config.yaml`, `docs/configuration.md`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalSensorConfigTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // FSensorDetectorConfig
  float BandLoUm = 0.4f, BandHiUm = 0.7f;   // presets: eo 0.4-0.7, mwir_cooled 3-5, lwir_uncooled 8-12
  // FSensorModeConfig
  float AGCMaxDisplayGain = 40.0f;          // thermal (radiance) AGC only
  FSensorExposureConfig ThermalExposure;    // radiance AE: auto, -8 / 0 EV, grey 0.5, p99, lag 2, manual -1
  // FCamSimConfig::FOceanConfig
  float WaterTemperatureC = 15.0f;          // initial FOceanSurface water temperature
  // FEntityTypeEntry
  FString ThermalMaterial;                  // entity_types.<id>.thermal_material ("" = vehicle_paint)
  TOptional<float> ThermalOffsetK;          // entity_types.<id>.thermal_offset_k, [-50, 500]
  ```
  YAML: `sensor_modes.<mode>.detector.band_lo_um|band_hi_um`, `sensor_modes.<mode>.agc_max_display_gain`, `sensor_modes.<mode>.thermal_exposure.{auto,min_gain_ev,max_photon_gain_ev,target_grey,highlight_percentile,lag_frames,manual_gain_ev}`, `ocean.water_temperature_c`, `entity_types.<id>.thermal_material|thermal_offset_k`. Env: `CAMSIM_IR_PRESET`, `CAMSIM_CAPTURE_WIDTH`, `CAMSIM_CAPTURE_HEIGHT`, `CAMSIM_OCEAN_WATER_TEMPERATURE_C`.

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalSensorConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Entity/EntityTypeTable.h"
#include "Sensor/SensorPresets.h"

#include <limits>

// CamSim.Thermal.Config.*: the sensor, ocean and entity-type keys the thermal path reads (ROADMAP 4A).

namespace
{
	bool HasError(const TArray<FString>& Errors, const TCHAR* Needle)
	{
		return Errors.ContainsByPredicate([Needle](const FString& E) { return E.Contains(Needle); });
	}
	const FSensorModeConfig& Ir(const FCamSimConfig& C) { return C.SensorModeConfigs.FindChecked(ESensorMode::IR); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPresetBandsTest, "CamSim.Thermal.Config.PresetBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPresetBandsTest::RunTest(const FString& Parameters)
{
	struct FExpect { const TCHAR* Preset; float Lo, Hi; };
	for (const FExpect& E : { FExpect{ TEXT("eo_hd_cmos"), 0.4f, 0.7f }, FExpect{ TEXT("mwir_cooled"), 3.0f, 5.0f }, FExpect{ TEXT("lwir_uncooled"), 8.0f, 12.0f } })
	{
		FSensorModeConfig M;
		CamSimSensorPresets::Apply(E.Preset, M);
		TestEqual(*FString::Printf(TEXT("%s band_lo_um"), E.Preset), M.Detector.BandLoUm, E.Lo);
		TestEqual(*FString::Printf(TEXT("%s band_hi_um"), E.Preset), M.Detector.BandHiUm, E.Hi);
	}
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n  ir:\n    preset: mwir_cooled\n    detector:\n      band_lo_um: 3.4\n      band_hi_um: 4.9\n"));
	TestEqual(TEXT("override lo"), Ir(Cfg).Detector.BandLoUm, 3.4f);
	TestEqual(TEXT("override hi"), Ir(Cfg).Detector.BandHiUm, 4.9f);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	FCamSimConfig Bad = FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  ir:\n    detector:\n      band_lo_um: 5\n      band_hi_um: 3\n"));
	TestTrue(TEXT("lo >= hi rejected"), HasError(Bad.Validate(), TEXT("band_lo_um")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalExposureConfigTest, "CamSim.Thermal.Config.ThermalExposureAndAgcCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalExposureConfigTest::RunTest(const FString& Parameters)
{
	const FSensorModeConfig D;
	TestEqual(TEXT("agc_max_display_gain default"), D.AGCMaxDisplayGain, 40.0f);
	TestTrue (TEXT("thermal auto"), D.ThermalExposure.bAuto);
	TestEqual(TEXT("thermal min"), D.ThermalExposure.MinGainEv, -8.0f);
	TestEqual(TEXT("thermal max photon"), D.ThermalExposure.MaxPhotonGainEv, 0.0f);
	TestEqual(TEXT("thermal grey (300 K mid-range)"), D.ThermalExposure.TargetGrey, 0.5f);
	TestEqual(TEXT("thermal highlight"), D.ThermalExposure.HighlightPercentile, 0.99f);
	TestEqual(TEXT("thermal lag"), D.ThermalExposure.LagFrames, 2);
	TestEqual(TEXT("thermal manual"), D.ThermalExposure.ManualGainEv, -1.0f);
	TestEqual(TEXT("luminance exposure untouched"), D.Exposure.MaxPhotonGainEv, -6.0f);

	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n"
		"  ir:\n"
		"    agc_max_display_gain: 25\n"
		"    thermal_exposure:\n"
		"      auto: false\n"
		"      min_gain_ev: -6\n"
		"      max_photon_gain_ev: -0.5\n"
		"      target_grey: 0.4\n"
		"      highlight_percentile: 0.98\n"
		"      lag_frames: 3\n"
		"      manual_gain_ev: -2\n"));
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestEqual(TEXT("cap"), Ir(Cfg).AGCMaxDisplayGain, 25.0f);
	TestFalse(TEXT("auto"), Ir(Cfg).ThermalExposure.bAuto);
	TestEqual(TEXT("min"), Ir(Cfg).ThermalExposure.MinGainEv, -6.0f);
	TestEqual(TEXT("max"), Ir(Cfg).ThermalExposure.MaxPhotonGainEv, -0.5f);
	TestEqual(TEXT("grey"), Ir(Cfg).ThermalExposure.TargetGrey, 0.4f);
	TestEqual(TEXT("hi pct"), Ir(Cfg).ThermalExposure.HighlightPercentile, 0.98f);
	TestEqual(TEXT("lag"), Ir(Cfg).ThermalExposure.LagFrames, 3);
	TestEqual(TEXT("manual"), Ir(Cfg).ThermalExposure.ManualGainEv, -2.0f);

	TestTrue(TEXT("cap < 1 rejected"), HasError(FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  ir:\n    agc_max_display_gain: 0.5\n")).Validate(),
		TEXT("agc_max_display_gain")));
	TestTrue(TEXT("thermal min > max rejected"), HasError(FCamSimConfig::LoadFromYamlString(TEXT(
		"sensor_modes:\n  ir:\n    thermal_exposure:\n      min_gain_ev: 1\n      max_photon_gain_ev: 0\n")).Validate(), TEXT("thermal_exposure")));
	TestFalse(TEXT("built-in defaults valid"), HasError(FCamSimConfig::LoadFromYamlString(TEXT("")).Validate(), TEXT("thermal_exposure")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalIrPresetEnvTest, "CamSim.Thermal.Config.IrPresetAndCaptureEnv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalIrPresetEnvTest::RunTest(const FString& Parameters)
{
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_IR_PRESET"), TEXT("lwir_uncooled"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CAPTURE_WIDTH"), TEXT("1920"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CAPTURE_HEIGHT"), TEXT("1080"));
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("capture_width: 1280\ncapture_height: 720\nsensor_modes:\n  ir:\n    preset: mwir_cooled\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_IR_PRESET"), TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CAPTURE_WIDTH"), TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CAPTURE_HEIGHT"), TEXT(""));
	TestEqual(TEXT("preset name"), Ir(Cfg).Preset, FString(TEXT("lwir_uncooled")));
	TestTrue (TEXT("microbolometer"), Ir(Cfg).Detector.Type == ESensorDetectorType::Microbolometer);
	TestEqual(TEXT("LWIR band lo"), Ir(Cfg).Detector.BandLoUm, 8.0f);
	TestEqual(TEXT("width"), Cfg.CaptureWidth, 1920);
	TestEqual(TEXT("height"), Cfg.CaptureHeight, 1080);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_IR_PRESET"), TEXT("mwir_typo"));
	const FCamSimConfig Bad = FCamSimConfig::LoadFromYamlString(TEXT(""));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_IR_PRESET"), TEXT(""));
	TestTrue(TEXT("unknown env preset reported"), HasError(Bad.Validate(), TEXT("mwir_typo")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalWaterTemperatureTest, "CamSim.Thermal.Config.WaterTemperature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalWaterTemperatureTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("default"), FCamSimConfig::FOceanConfig().WaterTemperatureC, 15.0f);
	FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("ocean:\n  water_temperature_c: 9.5\n"));
	TestEqual(TEXT("yaml"), Cfg.Ocean.WaterTemperatureC, 9.5f);
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_WATER_TEMPERATURE_C"), TEXT("21"));
	Cfg = FCamSimConfig::LoadFromYamlString(TEXT("ocean:\n  water_temperature_c: 9.5\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_OCEAN_WATER_TEMPERATURE_C"), TEXT(""));
	TestEqual(TEXT("env"), Cfg.Ocean.WaterTemperatureC, 21.0f);
	FCamSimConfig Hot; Hot.Ocean.WaterTemperatureC = 45.0f;
	TestTrue(TEXT("45 C rejected"), HasError(Hot.Validate(), TEXT("ocean.water_temperature_c")));
	FCamSimConfig NaNCfg; NaNCfg.Ocean.WaterTemperatureC = std::numeric_limits<float>::quiet_NaN();
	TestTrue(TEXT("NaN rejected"), HasError(NaNCfg.Validate(), TEXT("ocean.water_temperature_c")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalEntityTypeKeysTest, "CamSim.Thermal.Config.EntityTypeKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalEntityTypeKeysTest::RunTest(const FString& Parameters)
{
	FEntityTypeTable Table;
	Table.LoadFromYamlString(TEXT(
		"entity_types:\n"
		"  \"2001\":\n"
		"    mesh: truck/ural_4320.glb\n"
		"    thermal_material: asphalt\n"
		"    thermal_offset_k: 12.5\n"
		"  \"3001\":\n"
		"    mesh: boat/mako_655.glb\n"
		"    thermal_offset_k: 9999\n"));
	const FEntityTypeEntry* Truck = Table.FindEntry(2001);
	const FEntityTypeEntry* Boat = Table.FindEntry(3001);
	if (!TestNotNull(TEXT("truck"), Truck) || !TestNotNull(TEXT("boat"), Boat)) return false;
	TestEqual(TEXT("truck material"), Truck->ThermalMaterial, FString(TEXT("asphalt")));
	TestTrue (TEXT("truck offset"), Truck->ThermalOffsetK.IsSet() && *Truck->ThermalOffsetK == 12.5f);
	TestTrue (TEXT("boat material empty (vehicle_paint)"), Boat->ThermalMaterial.IsEmpty());
	TestFalse(TEXT("out-of-range offset ignored"), Boat->ThermalOffsetK.IsSet());
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile errors (`BandLoUm`, `AGCMaxDisplayGain`, `ThermalExposure`, `WaterTemperatureC`, `ThermalMaterial` are not members).

- [ ] **Step 3: Implement the sensor fields.** In `Sensor/SensorTypes.h`, in `FSensorDetectorConfig` after `DeadPixelFraction`:

```cpp
	/** Spectral band (micrometres) of the thermal radiance integral (ROADMAP 4A). The preset sets it. */
	float BandLoUm          = 0.4f;
	float BandHiUm          = 0.7f;
```

In `FSensorModeConfig`, after `int32 AGCLagFrames = 0;`:

```cpp
	/** ROADMAP 4A: highest display gain (normalised DN) the thermal (radiance) IR AGC may use; the band is
	 *  centred on mid-grey when the cap binds. The 3B.2 luminance proxy AGC is not capped. */
	float AGCMaxDisplayGain  = 40.0f;
```

and after `FSensorExposureConfig Exposure;`:

```cpp
	/** ROADMAP 4A: the AE for the thermal radiance input (signal = L / B(300 K), ~1 for a 300 K scene):
	 *  the photon gain exposes a 300 K scene to mid-range. Separate from Exposure, which the
	 *  thermal.enabled: false luminance proxy keeps using. */
	FSensorExposureConfig ThermalExposure = []
	{
		FSensorExposureConfig E;
		E.MinGainEv       = -8.0f;
		E.MaxPhotonGainEv = 0.0f;
		E.TargetGrey      = 0.5f;
		E.ManualGainEv    = -1.0f;
		return E;
	}();
```

In `Sensor/SensorPresets.cpp`, in the `eo_hd_cmos` branch after `DeadPixelFraction = 1e-5f;` add `InOut.Detector.BandLoUm = 0.4f; InOut.Detector.BandHiUm = 0.7f;`; in `mwir_cooled` after its `DeadPixelFraction` add `InOut.Detector.BandLoUm = 3.0f; InOut.Detector.BandHiUm = 5.0f;`; in `lwir_uncooled` after its `DeadPixelFraction` add `InOut.Detector.BandLoUm = 8.0f; InOut.Detector.BandHiUm = 12.0f;`.

- [ ] **Step 4: Implement the config keys.** In `Config/CamSimConfig.h`, in `FOceanConfig` after `MaxRadiusKm`:

```cpp
		float   WaterTemperatureC = 15.0f;    // initial water temperature (thermal water class) until CIGI Maritime Surface sets it
```

In `Config/CamSimConfig.cpp` `ParseMode`: replace the `if (YamlHas(ModeNode, "exposure")) { … }` block with a shared parser and add the new keys:

```cpp
				auto ParseExposure = [](ryml::ConstNodeRef ENode, FSensorExposureConfig& X)
				{
					YamlBool (ENode, "auto",                 X.bAuto);
					YamlFloat(ENode, "min_gain_ev",          X.MinGainEv);
					YamlFloat(ENode, "max_photon_gain_ev",   X.MaxPhotonGainEv);
					YamlFloat(ENode, "target_grey",          X.TargetGrey);
					YamlFloat(ENode, "highlight_percentile", X.HighlightPercentile);
					YamlInt  (ENode, "lag_frames",           X.LagFrames);
					YamlFloat(ENode, "manual_gain_ev",       X.ManualGainEv);
				};
				if (YamlHas(ModeNode, "exposure")) ParseExposure(ModeNode["exposure"], MC.Exposure);
				// ROADMAP 4A: thermal radiance AE + AGC cap
				if (YamlHas(ModeNode, "thermal_exposure")) ParseExposure(ModeNode["thermal_exposure"], MC.ThermalExposure);
				YamlFloat(ModeNode, "agc_max_display_gain", MC.AGCMaxDisplayGain);
```

In the `detector` block after `dead_pixel_fraction`:

```cpp
					YamlFloat(DNode, "band_lo_um",           MC.Detector.BandLoUm);
					YamlFloat(DNode, "band_hi_um",           MC.Detector.BandHiUm);
```

In the `ocean` block after `max_radius_km`:

```cpp
			YamlFloat (O, "water_temperature_c", Cfg.Ocean.WaterTemperatureC);
```

In `ApplyEnvOverrides`, after the thermal env block (Task 3):

```cpp
	// ROADMAP 4A acceptance: MWIR/LWIR and 1080p runs without editing the yaml.
	Cfg.CaptureWidth  = GetEnvInt(TEXT("CAMSIM_CAPTURE_WIDTH"),  Cfg.CaptureWidth);
	Cfg.CaptureHeight = GetEnvInt(TEXT("CAMSIM_CAPTURE_HEIGHT"), Cfg.CaptureHeight);
	Cfg.Ocean.WaterTemperatureC = GetEnvFloat(TEXT("CAMSIM_OCEAN_WATER_TEMPERATURE_C"), Cfg.Ocean.WaterTemperatureC);
	{
		const FString IrPreset = GetEnv(TEXT("CAMSIM_IR_PRESET"), FString());
		FSensorModeConfig* IrM = Cfg.SensorModeConfigs.Find(ESensorMode::IR);
		if (!IrPreset.IsEmpty() && IrM)
		{
			// Re-applies the whole preset (yaml optics:/detector: overrides are dropped); an unknown
			// name keeps the current values and Validate() reports it.
			IrM->Preset = IrPreset;
			CamSimSensorPresets::Apply(IrPreset, *IrM);
		}
	}
```

In `Validate()`, inside the `for (const TPair<ESensorMode, FSensorModeConfig>& Pair : SensorModeConfigs)` loop, after the existing `highlight_percentile` check:

```cpp
		// ROADMAP 4A: thermal radiance AE, AGC cap, band
		if (!(M.ThermalExposure.MinGainEv >= -40.0f))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].thermal_exposure: min_gain_ev (%.1f) must be >= -40"), ModeId, M.ThermalExposure.MinGainEv));
		if (!(M.ThermalExposure.MaxPhotonGainEv >= M.ThermalExposure.MinGainEv))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].thermal_exposure: min_gain_ev (%.1f) > max_photon_gain_ev (%.1f)"),
				ModeId, M.ThermalExposure.MinGainEv, M.ThermalExposure.MaxPhotonGainEv));
		if (!(M.ThermalExposure.HighlightPercentile > 0.0f && M.ThermalExposure.HighlightPercentile <= 1.0f))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].thermal_exposure: highlight_percentile=%.3f out of (0, 1]"), ModeId, M.ThermalExposure.HighlightPercentile));
		if (!(M.AGCMaxDisplayGain >= 1.0f) || !FMath::IsFinite(M.AGCMaxDisplayGain))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].agc_max_display_gain=%.2f must be finite and >= 1"), ModeId, M.AGCMaxDisplayGain));
		if (!(M.Detector.BandLoUm > 0.0f && M.Detector.BandHiUm > M.Detector.BandLoUm && M.Detector.BandHiUm <= 30.0f))
			Errors.Add(FString::Printf(TEXT("sensor_modes[%d].detector: band_lo_um (%.2f) / band_hi_um (%.2f) must satisfy 0 < lo < hi <= 30"),
				ModeId, M.Detector.BandLoUm, M.Detector.BandHiUm));
```

and after the ocean checks:

```cpp
	if (!(Ocean.WaterTemperatureC >= -2.0f && Ocean.WaterTemperatureC <= 40.0f))
		Errors.Add(FString::Printf(TEXT("ocean.water_temperature_c=%.2f out of range [-2, 40]"), Ocean.WaterTemperatureC));
```

- [ ] **Step 5: Implement the entity-type keys.** In `Entity/EntityTypeTable.h`, in `FEntityTypeEntry` after `EntityCategory`:

```cpp
	// ROADMAP 4A — thermal class for this type's pixels (FThermalMaterialTable name; empty = vehicle_paint)
	// and a temperature offset in K (unset = +8 K for land/sea vehicles, 0 otherwise).
	FString ThermalMaterial;
	TOptional<float> ThermalOffsetK;
```

In `Entity/EntityTypeTable.cpp` `LoadFromYamlString`, after `YamlString(EntryNode, "entity_category", Entry.EntityCategory);`:

```cpp
		// ROADMAP 4A — thermal class + offset (the class name is resolved by FThermalFrameBuilder, which warns once on an unknown name)
		YamlString(EntryNode, "thermal_material", Entry.ThermalMaterial);
		float OffsetK = 0.0f;
		if (YamlFloat(EntryNode, "thermal_offset_k", OffsetK))
		{
			if (FMath::IsFinite(OffsetK) && OffsetK >= -50.0f && OffsetK <= 500.0f)
			{
				Entry.ThermalOffsetK = OffsetK;
			}
			else
			{
				UE_LOG(LogCamSim, Warning, TEXT("EntityTypeTable: type %u thermal_offset_k %.2f out of range [-50, 500]; ignored"), TypeId, OffsetK);
			}
		}
```

- [ ] **Step 6: Initial water temperature.** In `Subsystem/CamSimSubsystem.cpp`, in the ocean creation block, after `ApplyOceanConfig(Config.Ocean);` (inside `if (CamSim::Geospatial::GetGeoidUndulation(0.0, 0.0).IsSet())`):

```cpp
			// ROADMAP 4A: the thermal water class's temperature until CIGI Maritime Surface Conditions sets one
			// (startup only, so a hot reload never overrides the host's value).
			Impl->Ocean->SetWaterTempC(Config.Ocean.WaterTemperatureC);
```

- [ ] **Step 7: Canonical yaml and docs.** In `deploy/camsim_config.yaml`: in `sensor_modes.eo`'s commented `detector:` block add `#   band_lo_um: 0.4` and `#   band_hi_um: 0.7`; in `sensor_modes.ir`'s commented `detector:` block add `#   band_lo_um: 3.0              # thermal radiance band (ROADMAP 4A)` and `#   band_hi_um: 5.0`; extend the `lwir_uncooled would instead set` comment's detector map with `band_lo_um: 8.0, band_hi_um: 12.0`; after `agc_lag_frames: 2` add:

```yaml
    agc_max_display_gain: 40.0  # ROADMAP 4A: cap on the thermal (radiance) AGC's display gain
```

after the IR `exposure:` block add:

```yaml
    # ROADMAP 4A — thermal radiance AE (signal = L / B(300 K)): exposes a 300 K scene to mid-range.
    # The exposure: block above stays in use for thermal.enabled: false (luminance proxy).
    thermal_exposure:
      min_gain_ev: -8
      max_photon_gain_ev: 0
      target_grey: 0.5
      highlight_percentile: 0.99
      lag_frames: 2
      manual_gain_ev: -1
```

in `ocean:` after `max_radius_km` add `  water_temperature_c: 15.0             # CAMSIM_OCEAN_WATER_TEMPERATURE_C — initial; CIGI Maritime Surface overrides`; in `entity_types` under `"2001"` and `"3001"` add the comment line `    # thermal_material: vehicle_paint  # ROADMAP 4A default; thermal_offset_k defaults to +8 K for land/sea vehicles`.

In `docs/configuration.md`:
- Sensor Modes table: add rows `| `agc_max_display_gain` | float | `40` | `40` | ROADMAP 4A. Highest display gain of the **thermal** IR AGC (normalised DN); when it binds, the band is centred on mid-grey. The 3B.2 luminance proxy AGC is not capped. Must be ≥ 1. |`.
- After the `exposure` table add a `**`sensor_modes.<mode>.thermal_exposure`**` paragraph and table with the same fields and defaults `true / -8 / 0 / 0.5 / 0.99 / 2 / -1`, stating it is the AE for the thermal radiance input (signal = L / B(300 K)) and that `exposure` keeps serving `thermal.enabled: false`.
- Presets table: add the row `| `detector.band_lo_um` / `band_hi_um` | 0.4 / 0.7 | 3 / 5 | 8 / 12 |`; override-keys table: `| `detector.band_lo_um`, `detector.band_hi_um` | float | Thermal radiance band in µm (ROADMAP 4A); `0 < lo < hi ≤ 30`. |`; validation list: `band_lo_um`/`band_hi_um`, `agc_max_display_gain < 1`, `thermal_exposure` like `exposure`.
- Under "Sensor Modes": `CAMSIM_IR_PRESET` re-applies the named preset to the IR mode after the yaml (dropping yaml `optics:`/`detector:` overrides); an unknown name is a validation error.
- Capture table: `capture_width`/`capture_height` env `CAMSIM_CAPTURE_WIDTH`/`CAMSIM_CAPTURE_HEIGHT` (restart only).
- Ocean table: `| `ocean.water_temperature_c` | `CAMSIM_OCEAN_WATER_TEMPERATURE_C` | `15.0` | Initial water temperature for the thermal water class (ROADMAP 4A); CIGI Maritime Surface Conditions overrides it. Startup only. `[-2, 40]`. |`
- Entity Types section: `thermal_material` (string, `FThermalMaterialTable` class name, default `vehicle_paint`; an unknown name falls back with one warning) and `thermal_offset_k` (float K, default +8 for land/sea vehicles — entities placed on the surface — else 0; outside `[-50, 500]` is ignored with a warning).

- [ ] **Step 8: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Thermal.Config && run_tests CamSim.Sensor && run_tests CamSim.Config && run_tests CamSim.Ocean` → Expected: `CamSim.Thermal.Config` 10 succeeded; the other three filters 0 failed (`PerModeExposureDefaults`/`…CanonicalConfig` unchanged).

- [ ] **Step 9: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorTypes.h \
  unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorPresets.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
  unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Entity/EntityTypeTable.h \
  unreal_project/CamSimTest/Source/CamSimTest/Entity/EntityTypeTable.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalSensorConfigTest.cpp \
  deploy/camsim_config.yaml docs/configuration.md
git commit -F - <<'EOF'
feat(thermal): preset bands, thermal AE/AGC cap, water temperature and entity thermal keys

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 5: Sky temperature (`FThermalSky`)

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalSky.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalSky.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalSkyTest.cpp`

**Interfaces:**
- Consumes: `FBandRadiance::Radiance` (Task 2).
- Produces:
  ```cpp
  struct CAMSIMTEST_API FThermalSky
  {
      static constexpr double MinSinEl    = 0.05;
      static constexpr int32  HemiSamples = 64;
      static constexpr double Sigma       = 5.670374419e-8;
      static double ClearZenithK(double TairK);                       // Swinbank 0.0552 T^1.5
      static double ZenithEmissivity(double TairK);                   // (T_clear / T_air)^4, clamped to [0, 1]
      static double Emissivity(double EpsZ, double SinEl);            // 1 - (1 - eps_z)^(1 / max(sin el, 0.05))
      static double SkyTemperatureK(double TairK, double Cloud, double SinEl);   // T_air ((1-c) eps(el) + c)^(1/4)
      static double HemisphericEmissivity(double TairK);              // cosine-weighted mean of eps(el)
      static double DownwellingIrradiance(double TairK, double Cloud);           // W m^-2, broadband
      static double HemisphereBandRadiance(const FBandRadiance& Band, double TairK, double Cloud);   // cosine-weighted mean of B(T_sky(el))
  };
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalSkyTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/BandRadiance.h"
#include "Thermal/ThermalSky.h"

// CamSim.Thermal.Sky.*: effective sky temperature (ROADMAP 4A).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSkyZenithTest, "CamSim.Thermal.Sky.ZenithColderThanHorizon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSkyZenithTest::RunTest(const FString& Parameters)
{
	const double Ta = 288.15;
	TestNearlyEqual(TEXT("Swinbank clear zenith"), FThermalSky::ClearZenithK(Ta), 270.002, 0.01);
	TestNearlyEqual(TEXT("zenith = clear zenith"), FThermalSky::SkyTemperatureK(Ta, 0.0, 1.0), FThermalSky::ClearZenithK(Ta), 1e-6);
	TestNearlyEqual(TEXT("horizon ~ T_air"), FThermalSky::SkyTemperatureK(Ta, 0.0, 0.05), Ta, 0.01);
	double Prev = FThermalSky::SkyTemperatureK(Ta, 0.0, 0.05);
	for (int32 I = 1; I <= 10; ++I)
	{
		const double SinEl = 0.05 + I * 0.095;
		const double T = FThermalSky::SkyTemperatureK(Ta, 0.0, SinEl);
		TestTrue(*FString::Printf(TEXT("colder toward the zenith (sin el %.3f)"), SinEl), T < Prev);
		Prev = T;
	}
	TestNearlyEqual(TEXT("below the horizon clamps to MinSinEl"), FThermalSky::SkyTemperatureK(Ta, 0.0, -0.7),
		FThermalSky::SkyTemperatureK(Ta, 0.0, FThermalSky::MinSinEl), 1e-9);
	TestEqual(TEXT("zenith emissivity clamps to 1 for a very hot day"), FThermalSky::ZenithEmissivity(400.0), 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSkyOvercastTest, "CamSim.Thermal.Sky.OvercastIsAirTemperature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSkyOvercastTest::RunTest(const FString& Parameters)
{
	const double Ta = 283.0;
	for (const double SinEl : { -0.2, 0.05, 0.5, 1.0 })
	{
		TestNearlyEqual(*FString::Printf(TEXT("overcast at sin el %.2f"), SinEl), FThermalSky::SkyTemperatureK(Ta, 1.0, SinEl), Ta, 1e-9);
	}
	TestNearlyEqual(TEXT("overcast downwelling = sigma T^4"), FThermalSky::DownwellingIrradiance(Ta, 1.0), FThermalSky::Sigma * Ta * Ta * Ta * Ta, 1e-6);
	FBandRadiance Band;
	Band.Build(8.0, 12.0);
	const double B = Band.Radiance(static_cast<float>(Ta));
	TestNearlyEqual(TEXT("overcast hemisphere radiance = B(T_air)"), FThermalSky::HemisphereBandRadiance(Band, Ta, 1.0), B, B * 1e-5);
	TestTrue(TEXT("clear hemisphere radiance below B(T_air)"), FThermalSky::HemisphereBandRadiance(Band, Ta, 0.0) < B);
	TestTrue(TEXT("half cloud between"), FThermalSky::HemisphereBandRadiance(Band, Ta, 0.5) > FThermalSky::HemisphereBandRadiance(Band, Ta, 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSkyHemisphereTest, "CamSim.Thermal.Sky.HemisphericEmissivity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSkyHemisphereTest::RunTest(const FString& Parameters)
{
	const double Ta = 288.15;
	const double EpsZ = FThermalSky::ZenithEmissivity(Ta);
	const double EpsH = FThermalSky::HemisphericEmissivity(Ta);
	TestNearlyEqual(TEXT("zenith emissivity"), EpsZ, 0.77089, 1e-4);
	TestTrue(TEXT("eps_z < eps_hemi < 1"), EpsZ < EpsH && EpsH < 1.0);
	TestNearlyEqual(TEXT("eps_hemi (64-sample cosine-weighted mean)"), EpsH, 0.8827, 0.005);
	TestNearlyEqual(TEXT("clear downwelling"), FThermalSky::DownwellingIrradiance(Ta, 0.0), FThermalSky::Sigma * EpsH * Ta * Ta * Ta * Ta, 1e-6);
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/ThermalSky.h` not found.

- [ ] **Step 3: Implement** — `Source/CamSimTest/Thermal/ThermalSky.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FBandRadiance;

/**
 * Effective sky temperature (ROADMAP 4A): Swinbank clear-sky zenith T_clear = 0.0552 T_air^1.5, angular
 * emissivity eps(el) = 1 - (1 - eps_z)^(1 / max(sin el, 0.05)) with eps_z = (T_clear / T_air)^4, and cloud
 * cover blending T_sky^4 = (1 - c) eps(el) T_air^4 + c T_air^4. ThermalCS evaluates SkyTemperatureK per
 * sky pixel (CamSimThermalCommon.ush SkyTemperatureK, same expressions). Pure.
 */
struct CAMSIMTEST_API FThermalSky
{
	static constexpr double MinSinEl    = 0.05;
	static constexpr int32  HemiSamples = 64;    // midpoint rule over elevation, cosine-weighted
	static constexpr double Sigma       = 5.670374419e-8;

	static double ClearZenithK(double TairK) { return 0.0552 * FMath::Pow(TairK, 1.5); }
	static double ZenithEmissivity(double TairK);
	static double Emissivity(double EpsZ, double SinEl) { return 1.0 - FMath::Pow(1.0 - EpsZ, 1.0 / FMath::Max(SinEl, MinSinEl)); }
	static double SkyTemperatureK(double TairK, double Cloud, double SinEl);
	static double HemisphericEmissivity(double TairK);
	static double DownwellingIrradiance(double TairK, double Cloud);
	static double HemisphereBandRadiance(const FBandRadiance& Band, double TairK, double Cloud);
};
```

`Source/CamSimTest/Thermal/ThermalSky.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalSky.h"
#include "Thermal/BandRadiance.h"

namespace
{
	/** Midpoint elevations of the hemisphere integral and their cosine weights 2 sin(el) cos(el) d(el), normalised to sum 1. */
	template <typename TFn>
	double CosineWeightedMean(TFn&& Fn)
	{
		double Sum = 0.0, Weights = 0.0;
		const double DEl = 0.5 * UE_DOUBLE_PI / FThermalSky::HemiSamples;
		for (int32 I = 0; I < FThermalSky::HemiSamples; ++I)
		{
			const double El = (I + 0.5) * DEl;
			const double W = 2.0 * FMath::Sin(El) * FMath::Cos(El) * DEl;
			Sum += W * Fn(FMath::Sin(El));
			Weights += W;
		}
		return Sum / Weights;
	}
}

double FThermalSky::ZenithEmissivity(double TairK)
{
	const double R = ClearZenithK(TairK) / TairK;
	return FMath::Clamp(R * R * R * R, 0.0, 1.0);
}

double FThermalSky::SkyTemperatureK(double TairK, double Cloud, double SinEl)
{
	const double C = FMath::Clamp(Cloud, 0.0, 1.0);
	const double Ratio = (1.0 - C) * Emissivity(ZenithEmissivity(TairK), SinEl) + C;
	return TairK * FMath::Sqrt(FMath::Sqrt(Ratio));
}

double FThermalSky::HemisphericEmissivity(double TairK)
{
	const double EpsZ = ZenithEmissivity(TairK);
	return CosineWeightedMean([EpsZ](double SinEl) { return Emissivity(EpsZ, SinEl); });
}

double FThermalSky::DownwellingIrradiance(double TairK, double Cloud)
{
	const double C = FMath::Clamp(Cloud, 0.0, 1.0);
	return Sigma * ((1.0 - C) * HemisphericEmissivity(TairK) + C) * TairK * TairK * TairK * TairK;
}

double FThermalSky::HemisphereBandRadiance(const FBandRadiance& Band, double TairK, double Cloud)
{
	return CosineWeightedMean([&Band, TairK, Cloud](double SinEl)
	{
		return static_cast<double>(Band.Radiance(static_cast<float>(SkyTemperatureK(TairK, Cloud, SinEl))));
	});
}
```

- [ ] **Step 4: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Thermal.Sky` → Expected: `succeeded 3 failed 0`.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalSky.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalSky.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalSkyTest.cpp
git commit -F - <<'EOF'
feat(thermal): Swinbank sky temperature with angular emissivity and cloud blend

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 6: Closed-form class temperatures (`FThermalModel`)

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalModel.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalModel.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalModelTest.cpp`

**Interfaces:**
- Consumes: `FThermalMaterial`, `FThermalMaterialTable` (Task 3); `FThermalSky::DownwellingIrradiance` (Task 5); `ACamSimEnvironment::ComputeSunPosition(float Hour, int32 DayOfYear, double Latitude)` (existing, `Environment/CamSimEnvironment.h`; returns `(elevation, azimuth)` degrees for local solar time).
- Produces:
  ```cpp
  struct FThermalSite { int32 Year = 2026; int32 DayOfYear = 172; double LatDeg = 0.0, LonDeg = 0.0; double TairMeanK = 288.15; double AirSwingK = 8.0; double Cloud = 0.0; };
  class CAMSIMTEST_API FThermalModel
  {
  public:
      static constexpr int32  NumSamples = 96, NumHarmonics = 6;
      static constexpr double DaySeconds = 86400.0, Omega = 2 pi / DaySeconds, AirPeakHour = 15.0, WaterSwingK = 0.5;
      static constexpr double Sigma = 5.670374419e-8, RefitDegrees = 0.5;
      struct FHarmonics { double Re[NumHarmonics + 1] = {}; double Im[NumHarmonics + 1] = {}; };
      static double SunElevationDeg(const FThermalSite& S, double LocalSolarHour);
      static double ClearSkyGhi(double SunElevationDeg);          // Haurwitz, W m^-2
      static double CloudFactor(double Cloud);                    // Kasten-Czeplak 1 - 0.75 c^3.4
      static double AirTemperatureK(const FThermalSite& S, double LocalSolarHour);
      static double ExchangeCoefficient(const FThermalSite& S, const FThermalMaterial& M);   // h_c + 4 eps sigma T_mean^3
      static double Forcing(const FThermalSite& S, const FThermalMaterial& M, double LocalSolarHour);
      static FHarmonics Fit(TConstArrayView<double> Samples);     // NumSamples samples, every 15 min from local solar midnight
      static double Response(const FHarmonics& F, double H, double Inertia, double LocalSolarSec);
      bool   Update(const FThermalSite& Site, const FThermalMaterialTable& Materials);   // true when it refit
      double TemperatureK(int32 Class, double LocalSolarSec, double WaterTempK) const;
      const FThermalSite& GetSite() const;
      bool   IsFitted() const;
  };
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalModelTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/ThermalMaterials.h"
#include "Thermal/ThermalModel.h"

// CamSim.Thermal.Model.*: the closed-form surface temperature (ROADMAP 4A).

namespace
{
	constexpr int32 N = FThermalModel::NumSamples;

	FThermalSite SanFrancisco(int32 DayOfYear)
	{
		FThermalSite S;
		S.Year = 2026; S.DayOfYear = DayOfYear; S.LatDeg = 37.80; S.LonDeg = -122.45;
		S.TairMeanK = 288.15; S.AirSwingK = 8.0; S.Cloud = 0.0;
		return S;
	}

	/** Smooth forcing: exactly representable by NumHarmonics harmonics. Peak of the first harmonic at PeakHour. */
	TArray<double> CosineForcing(double Mean, double Amp, double PeakHour)
	{
		TArray<double> F;
		for (int32 K = 0; K < N; ++K)
		{
			const double Th = 2.0 * UE_DOUBLE_PI * K / N;
			F.Add(Mean + Amp * FMath::Cos(Th - 2.0 * UE_DOUBLE_PI * PeakHour / 24.0) + 0.1 * Amp * FMath::Cos(3.0 * Th));
		}
		return F;
	}

	/** (min, max, hour of max) of a response over a day at 1-minute steps. */
	FVector3d DailyRange(TFunctionRef<double(double)> T)
	{
		double Lo = TNumericLimits<double>::Max(), Hi = -Lo, HiHour = 0.0;
		for (int32 M = 0; M < 1440; ++M)
		{
			const double V = T(M * 60.0);
			Lo = FMath::Min(Lo, V);
			if (V > Hi) { Hi = V; HiHour = M / 60.0; }
		}
		return FVector3d(Lo, Hi, HiHour);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelEquilibriumTest, "CamSim.Thermal.Model.ZeroInertiaTracksEquilibrium",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelEquilibriumTest::RunTest(const FString& Parameters)
{
	const TArray<double> F = CosineForcing(5000.0, 300.0, 13.0);
	const FThermalModel::FHarmonics H = FThermalModel::Fit(F);
	const double Hc = 15.0;
	for (int32 K = 0; K < N; K += 5)
	{
		TestNearlyEqual(*FString::Printf(TEXT("I = 0 tracks F/h at sample %d"), K), FThermalModel::Response(H, Hc, 0.0, K * 900.0), F[K] / Hc, 1e-9);
	}
	// Real forcing (sunrise kink, so only up to the 6-harmonic truncation): noon within 2 K.
	const FThermalSite Site = SanFrancisco(355);
	FThermalMaterial Soil = FThermalMaterialTable::BuiltIns()[FThermalMaterialTable::TerrainDefault];
	Soil.ThermalInertia = 0.0f;
	TArray<double> Fr;
	for (int32 K = 0; K < N; ++K) Fr.Add(FThermalModel::Forcing(Site, Soil, K * 24.0 / N));
	const double Hs = FThermalModel::ExchangeCoefficient(Site, Soil);
	TestNearlyEqual(TEXT("real forcing, noon"), FThermalModel::Response(FThermalModel::Fit(Fr), Hs, 0.0, 43200.0), Fr[N / 2] / Hs, 2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelInertiaTest, "CamSim.Thermal.Model.InertiaDampsAndDelays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelInertiaTest::RunTest(const FString& Parameters)
{
	const FThermalModel::FHarmonics H = FThermalModel::Fit(CosineForcing(5000.0, 400.0, 12.0));
	double PrevAmp = TNumericLimits<double>::Max(), PrevPeak = -1.0;
	for (const double I : { 0.0, 500.0, 1500.0, 3000.0 })
	{
		const FVector3d R = DailyRange([&](double T) { return FThermalModel::Response(H, 15.0, I, T); });
		const double Amp = 0.5 * (R.Y - R.X);
		TestTrue(*FString::Printf(TEXT("amplitude falls with inertia (I %.0f: %.3f K)"), I, Amp), Amp < PrevAmp);
		TestTrue(*FString::Printf(TEXT("peak moves later with inertia (I %.0f: %.2f h)"), I, R.Z), R.Z > PrevPeak);
		PrevAmp = Amp;
		PrevPeak = R.Z;
	}
	TestTrue(TEXT("I = 3000 lags noon by more than 1.5 h"), PrevPeak > 13.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelMeanTest, "CamSim.Thermal.Model.DailyMeanIsF0OverH",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelMeanTest::RunTest(const FString& Parameters)
{
	const FThermalSite Site = SanFrancisco(172);
	const FThermalMaterial Soil = FThermalMaterialTable::BuiltIns()[FThermalMaterialTable::TerrainDefault];
	TArray<double> F;
	for (int32 K = 0; K < N; ++K) F.Add(FThermalModel::Forcing(Site, Soil, K * 24.0 / N));
	const FThermalModel::FHarmonics H = FThermalModel::Fit(F);
	const double Hs = FThermalModel::ExchangeCoefficient(Site, Soil);
	for (const double I : { 0.0, 800.0, 2500.0 })
	{
		double Mean = 0.0;
		for (int32 M = 0; M < 1440; ++M) Mean += FThermalModel::Response(H, Hs, I, M * 60.0) / 1440.0;
		TestNearlyEqual(*FString::Printf(TEXT("daily mean = F0 / h at I %.0f"), I), Mean, H.Re[0] / Hs, 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelDeterminismTest, "CamSim.Thermal.Model.DeterministicUnderClockJumps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelDeterminismTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable Table;
	FThermalModel A, B;
	TestTrue(TEXT("first update fits"), A.Update(SanFrancisco(355), Table));
	const double Noon = A.TemperatureK(FThermalMaterialTable::TerrainDefault, 43200.0, 288.15);
	A.TemperatureK(FThermalMaterialTable::TerrainDefault, 3600.0, 288.15);
	A.TemperatureK(FThermalMaterialTable::TerrainDefault, 86000.0, 288.15);
	TestEqual(TEXT("no history: same time, same value after jumps"), A.TemperatureK(FThermalMaterialTable::TerrainDefault, 43200.0, 288.15), Noon);
	FThermalSite Other = SanFrancisco(172);
	B.Update(Other, Table);
	B.Update(SanFrancisco(355), Table);
	TestEqual(TEXT("a model that saw another day first agrees"), B.TemperatureK(FThermalMaterialTable::TerrainDefault, 43200.0, 288.15), Noon);
	TestFalse(TEXT("identical site: no refit"), A.Update(SanFrancisco(355), Table));
	FThermalSite Near = SanFrancisco(355); Near.LatDeg += 0.4;
	TestFalse(TEXT("0.4 deg move: no refit"), A.Update(Near, Table));
	FThermalSite Far = SanFrancisco(355); Far.LatDeg += 0.6;
	TestTrue(TEXT("0.6 deg move: refit"), A.Update(Far, Table));
	FThermalSite Cloudy = Far; Cloudy.Cloud = 0.3;
	TestTrue(TEXT("cloud change: refit"), A.Update(Cloudy, Table));
	FThermalSite Warm = Cloudy; Warm.TairMeanK += 1.0;
	TestTrue(TEXT("air temperature change: refit"), A.Update(Warm, Table));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelCrossoverTest, "CamSim.Thermal.Model.CrossoverWaterVsSoil",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelCrossoverTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable Table;
	FThermalModel M;
	M.Update(SanFrancisco(355), Table);
	const double WaterK = 288.15;
	const double SoilNoon = M.TemperatureK(FThermalMaterialTable::TerrainDefault, 12.0 * 3600.0, WaterK);
	const double SoilNight = M.TemperatureK(FThermalMaterialTable::TerrainDefault, 4.0 * 3600.0, WaterK);
	const double WaterNoon = M.TemperatureK(FThermalMaterialTable::Water, 12.0 * 3600.0, WaterK);
	const double WaterNight = M.TemperatureK(FThermalMaterialTable::Water, 4.0 * 3600.0, WaterK);
	TestTrue(*FString::Printf(TEXT("noon: soil %.1f K warmer than water %.1f K"), SoilNoon, WaterNoon), SoilNoon > WaterNoon + 2.0);
	TestTrue(*FString::Printf(TEXT("04:00: soil %.1f K cooler than water %.1f K"), SoilNight, WaterNight), SoilNight < WaterNight - 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelCloudsTest, "CamSim.Thermal.Model.CloudsDampAmplitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelCloudsTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable Table;
	FThermalModel Clear, Overcast;
	FThermalSite S = SanFrancisco(172);
	Clear.Update(S, Table);
	S.Cloud = 0.9;
	Overcast.Update(S, Table);
	const FVector3d C = DailyRange([&](double T) { return Clear.TemperatureK(FThermalMaterialTable::TerrainDefault, T, 288.15); });
	const FVector3d O = DailyRange([&](double T) { return Overcast.TemperatureK(FThermalMaterialTable::TerrainDefault, T, 288.15); });
	TestTrue(*FString::Printf(TEXT("overcast range %.1f K < clear range %.1f K"), O.Y - O.X, C.Y - C.X), (O.Y - O.X) < (C.Y - C.X));
	TestNearlyEqual(TEXT("Kasten-Czeplak factor at c = 1"), FThermalModel::CloudFactor(1.0), 0.25, 1e-12);
	TestEqual(TEXT("no sun below the horizon"), FThermalModel::ClearSkyGhi(-3.0), 0.0);
	TestNearlyEqual(TEXT("Haurwitz at the zenith"), FThermalModel::ClearSkyGhi(90.0), 1098.0 * FMath::Exp(-0.057), 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelFixedTest, "CamSim.Thermal.Model.FixedTemperatureClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelFixedTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable Table;
	FThermalModel M;
	M.Update(SanFrancisco(172), Table);
	const double WaterK = 285.0;
	const FVector3d R = DailyRange([&](double T) { return M.TemperatureK(FThermalMaterialTable::Water, T, WaterK); });
	TestNearlyEqual(TEXT("water min"), R.X, WaterK - FThermalModel::WaterSwingK, 1e-3);
	TestNearlyEqual(TEXT("water max"), R.Y, WaterK + FThermalModel::WaterSwingK, 1e-3);
	TestNearlyEqual(TEXT("water peaks at 15:00"), R.Z, FThermalModel::AirPeakHour, 0.02);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelPolarTest, "CamSim.Thermal.Model.PolarDayAndNight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelPolarTest::RunTest(const FString& Parameters)
{
	// Review focus 3: a sun that never sets (S > 0 all day) or never rises (S = 0 all day).
	const FThermalMaterialTable Table;
	for (const int32 Doy : { 172, 355 })
	{
		FThermalSite S = SanFrancisco(Doy);
		S.LatDeg = 80.0;
		S.TairMeanK = 273.15;
		FThermalModel M;
		M.Update(S, Table);
		for (int32 C = 0; C < Table.Num(); ++C)
		{
			const FVector3d R = DailyRange([&](double T) { return M.TemperatureK(C, T, 272.0); });
			TestTrue(*FString::Printf(TEXT("day %d class %s finite and in [150, 400] K (%.1f..%.1f)"), Doy, *Table.Get(C).Name, R.X, R.Y),
				FMath::IsFinite(R.X) && FMath::IsFinite(R.Y) && R.X >= 150.0 && R.Y <= 400.0);
		}
	}
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/ThermalModel.h` not found.

- [ ] **Step 3: Implement** — `Source/CamSimTest/Thermal/ThermalModel.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Thermal/ThermalMaterials.h"

/** Where and when the class temperatures are evaluated (ROADMAP 4A). Local solar time throughout. */
struct FThermalSite
{
	int32  Year      = 2026;
	int32  DayOfYear = 172;
	double LatDeg    = 0.0;
	double LonDeg    = 0.0;
	double TairMeanK = 288.15;   // daily mean air temperature
	double AirSwingK = 8.0;      // peak-to-peak
	double Cloud     = 0.0;      // [0, 1]
};

/**
 * Closed-form surface temperature per class (ROADMAP 4A). A class is a semi-infinite solid of thermal inertia
 * I with a linearised surface exchange h = h_c + 4 eps sigma T_mean^3. For the periodic forcing
 *   F(t) = (1 - a) S(t) (1 - 0.75 c^3.4) + h T_air(t) + eps (L_sky(t) - sigma T_air(t)^4)
 * sampled every 15 min over the day (S: Haurwitz clear-sky GHI at the sun elevation of
 * ACamSimEnvironment::ComputeSunPosition; L_sky: FThermalSky::DownwellingIrradiance) with Fourier
 * coefficients F_n (n = 0..6), the exact response is T(t) = F_0 / h + sum 2 Re[F_n e^{i n w t} H(n w)],
 * H(w) = 1 / (h + I sqrt(i w)). No integration state: any sim time evaluates directly. Water-source classes
 * are the water temperature +- WaterSwingK (peak 15:00). Game thread.
 */
class CAMSIMTEST_API FThermalModel
{
public:
	static constexpr int32  NumSamples   = 96;
	static constexpr int32  NumHarmonics = 6;
	static constexpr double DaySeconds   = 86400.0;
	static constexpr double Omega        = 2.0 * UE_DOUBLE_PI / DaySeconds;
	static constexpr double AirPeakHour  = 15.0;
	static constexpr double WaterSwingK  = 0.5;
	static constexpr double Sigma        = 5.670374419e-8;
	static constexpr double RefitDegrees = 0.5;

	/** F_n = Re[n] + i Im[n], n = 0..NumHarmonics (F_0 real). */
	struct FHarmonics
	{
		double Re[NumHarmonics + 1] = {};
		double Im[NumHarmonics + 1] = {};
	};

	static double SunElevationDeg(const FThermalSite& S, double LocalSolarHour);
	static double ClearSkyGhi(double SunElevationDeg);
	static double CloudFactor(double Cloud);
	static double AirTemperatureK(const FThermalSite& S, double LocalSolarHour);
	static double ExchangeCoefficient(const FThermalSite& S, const FThermalMaterial& M);
	static double Forcing(const FThermalSite& S, const FThermalMaterial& M, double LocalSolarHour);
	static FHarmonics Fit(TConstArrayView<double> Samples);
	static double Response(const FHarmonics& F, double H, double Inertia, double LocalSolarSec);

	/** Refit when the date, cloud cover, air temperature or materials change, or lat/lon move by more than RefitDegrees. */
	bool Update(const FThermalSite& NewSite, const FThermalMaterialTable& Materials);
	/** Class temperature at local solar second LocalSolarSec. Class is clamped to the fitted range. */
	double TemperatureK(int32 Class, double LocalSolarSec, double WaterTempK) const;
	const FThermalSite& GetSite() const { return Site; }
	bool IsFitted() const { return bFitted; }

private:
	struct FClass
	{
		bool   bWater  = false;
		double H       = 1.0;
		double Inertia = 0.0;
		FHarmonics F;
	};
	FThermalSite   Site;
	bool           bFitted = false;
	uint32         MaterialsVersion = 0;
	TArray<FClass> Classes;
};
```

`Source/CamSimTest/Thermal/ThermalModel.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalModel.h"
#include "Thermal/ThermalSky.h"
#include "Environment/CamSimEnvironment.h"

double FThermalModel::SunElevationDeg(const FThermalSite& S, double LocalSolarHour)
{
	const double Hour = FMath::Fmod(FMath::Fmod(LocalSolarHour, 24.0) + 24.0, 24.0);
	return ACamSimEnvironment::ComputeSunPosition(static_cast<float>(Hour), S.DayOfYear, S.LatDeg).X;
}

double FThermalModel::ClearSkyGhi(double SunElevationDeg)
{
	const double SinEl = FMath::Sin(FMath::DegreesToRadians(SunElevationDeg));
	if (!(SinEl > 0.0)) return 0.0;
	return 1098.0 * SinEl * FMath::Exp(-0.057 / SinEl);
}

double FThermalModel::CloudFactor(double Cloud)
{
	return 1.0 - 0.75 * FMath::Pow(FMath::Clamp(Cloud, 0.0, 1.0), 3.4);
}

double FThermalModel::AirTemperatureK(const FThermalSite& S, double LocalSolarHour)
{
	return S.TairMeanK + 0.5 * S.AirSwingK * FMath::Cos(Omega * (LocalSolarHour - AirPeakHour) * 3600.0);
}

double FThermalModel::ExchangeCoefficient(const FThermalSite& S, const FThermalMaterial& M)
{
	const double T = S.TairMeanK;
	return M.ConvectionWm2K + 4.0 * M.Emissivity * Sigma * T * T * T;
}

double FThermalModel::Forcing(const FThermalSite& S, const FThermalMaterial& M, double LocalSolarHour)
{
	const double Sun = ClearSkyGhi(SunElevationDeg(S, LocalSolarHour)) * CloudFactor(S.Cloud);
	const double Tair = AirTemperatureK(S, LocalSolarHour);
	const double Lsky = FThermalSky::DownwellingIrradiance(Tair, S.Cloud);
	return (1.0 - M.Albedo) * Sun + ExchangeCoefficient(S, M) * Tair + M.Emissivity * (Lsky - Sigma * Tair * Tair * Tair * Tair);
}

FThermalModel::FHarmonics FThermalModel::Fit(TConstArrayView<double> Samples)
{
	check(Samples.Num() == NumSamples);
	FHarmonics Out;
	for (int32 N = 0; N <= NumHarmonics; ++N)
	{
		double Re = 0.0, Im = 0.0;
		for (int32 K = 0; K < NumSamples; ++K)
		{
			const double Th = 2.0 * UE_DOUBLE_PI * N * K / NumSamples;
			Re += Samples[K] * FMath::Cos(Th);
			Im -= Samples[K] * FMath::Sin(Th);
		}
		Out.Re[N] = Re / NumSamples;
		Out.Im[N] = Im / NumSamples;
	}
	return Out;
}

double FThermalModel::Response(const FHarmonics& F, double H, double Inertia, double LocalSolarSec)
{
	double T = F.Re[0] / H;
	for (int32 N = 1; N <= NumHarmonics; ++N)
	{
		const double W = N * Omega;
		const double Q = Inertia * FMath::Sqrt(0.5 * W);   // I sqrt(i w) = Q (1 + i)
		const double A = H + Q, B = Q, D = A * A + B * B;  // H(w) = (A - i B) / D
		const double Hr = A / D, Hi = -B / D;
		const double Ph = W * LocalSolarSec;
		const double C = FMath::Cos(Ph), S = FMath::Sin(Ph);
		const double Er = F.Re[N] * C - F.Im[N] * S, Ei = F.Re[N] * S + F.Im[N] * C;   // F_n e^{i n w t}
		T += 2.0 * (Er * Hr - Ei * Hi);
	}
	return T;
}

bool FThermalModel::Update(const FThermalSite& NewSite, const FThermalMaterialTable& Materials)
{
	const bool bSame = bFitted
		&& NewSite.Year == Site.Year && NewSite.DayOfYear == Site.DayOfYear
		&& FMath::Abs(NewSite.LatDeg - Site.LatDeg) <= RefitDegrees
		&& FMath::Abs(NewSite.LonDeg - Site.LonDeg) <= RefitDegrees
		&& NewSite.Cloud == Site.Cloud && NewSite.TairMeanK == Site.TairMeanK && NewSite.AirSwingK == Site.AirSwingK
		&& Materials.GetVersion() == MaterialsVersion && Classes.Num() == Materials.Num();
	if (bSame) return false;

	Site = NewSite;
	MaterialsVersion = Materials.GetVersion();
	bFitted = true;

	// The site terms of the forcing, once per sample (the per-class part is a weighted sum of them).
	double Sun[NumSamples], Tair[NumSamples], Lsky[NumSamples];
	for (int32 K = 0; K < NumSamples; ++K)
	{
		const double Hour = K * 24.0 / NumSamples;
		Sun[K]  = ClearSkyGhi(SunElevationDeg(Site, Hour)) * CloudFactor(Site.Cloud);
		Tair[K] = AirTemperatureK(Site, Hour);
		Lsky[K] = FThermalSky::DownwellingIrradiance(Tair[K], Site.Cloud);
	}
	Classes.SetNum(Materials.Num());
	for (int32 C = 0; C < Materials.Num(); ++C)
	{
		const FThermalMaterial& M = Materials.Get(C);
		FClass& Out = Classes[C];
		Out.bWater  = M.Source == EThermalTemperatureSource::Water;
		Out.H       = ExchangeCoefficient(Site, M);
		Out.Inertia = M.ThermalInertia;
		if (Out.bWater) continue;
		double F[NumSamples];
		for (int32 K = 0; K < NumSamples; ++K)
		{
			const double T = Tair[K];
			F[K] = (1.0 - M.Albedo) * Sun[K] + Out.H * T + M.Emissivity * (Lsky[K] - Sigma * T * T * T * T);
		}
		Out.F = Fit(TConstArrayView<double>(F, NumSamples));
	}
	return true;
}

double FThermalModel::TemperatureK(int32 Class, double LocalSolarSec, double WaterTempK) const
{
	if (Classes.Num() == 0) return Site.TairMeanK;
	const FClass& C = Classes[FMath::Clamp(Class, 0, Classes.Num() - 1)];
	if (C.bWater)
	{
		return WaterTempK + WaterSwingK * FMath::Cos(Omega * (LocalSolarSec - AirPeakHour * 3600.0));
	}
	return Response(C.F, C.H, C.Inertia, LocalSolarSec);
}
```

- [ ] **Step 4: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Thermal.Model` → Expected: `succeeded 8 failed 0`.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalModel.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalModel.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalModelTest.cpp
git commit -F - <<'EOF'
feat(thermal): closed-form diurnal class temperatures (Fourier response of a semi-infinite solid)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 7: `FThermalFrameParams` and the per-pixel CPU reference (`CamSimThermalRef`)

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalFrameParams.h` (full struct)
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalReference.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalReference.cpp`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalTestScene.h` (header-only synthetic scene, shared with Task 8's GPU test)
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalReferenceTest.cpp`

**Interfaces:**
- Consumes: `ThermalLutRadiance` (Task 2), `FBandRadiance` (Task 2, tests only).
- Produces (`ThermalFrameParams.h`, fields added to Task 2's struct; constants unchanged):
  ```cpp
  static constexpr int32 MaxClasses = 32, NumStencils = 256;
  uint32 NumClasses; float ClassTempK[32], ClassEmissivity[32], ClassKFast[32], ClassSAbsRef[32]; uint32 TerrainClass, WaterClass;
  uint8 StencilClass[256]; float StencilOffsetK[256]; float EntityDepthRatio;
  FMatrix44f ClipToTranslatedWorld; FVector3f Up; uint32 bWater; float CamHeightCm, SeaRadiusCm, WaterBandCm;
  float TairK, SkyEpsZ, Cloud, SkyHemiRadiance, BetaPerCm;
  float KLum, EClampWm2, KFastScale; uint32 bBaseColorSrgb; float InputScale;
  ```
  and (`Thermal/ThermalReference.h`):
  ```cpp
  namespace CamSimThermalRef
  {
      enum class EPixelClass : uint8 { Sky, Entity, Water, Terrain, Invalid };
      struct FPixelSample { float U, V, DeviceZ, CustomZ; uint32 Stencil; FVector3f Color, Base; bool bHasBase; };
      struct FPixelResult { EPixelClass Class; float TempK; float Radiance; };
      struct FImages { int32 W, H, DepthW, DepthH; const TArray<FLinearColor>* SceneColor; const TArray<float>* SceneDepth;
                       const TArray<float>* CustomDepth; const TArray<uint8>* Stencil; const TArray<FLinearColor>* BaseColor; };
      float LutRadiance(const FThermalFrameParams& P, float TK);
      float SkyTemperatureK(const FThermalFrameParams& P, float SinEl);
      float SrgbToLinear(float C);
      FPixelResult EvaluatePixel(const FThermalFrameParams& P, const FPixelSample& S);
      TArray<FPixelResult> Run(const FImages& In, const FThermalFrameParams& P);
      FMatrix44f MakeClipToTranslatedWorld(const FRotator& ViewRotation, float HFovDeg, int32 W, int32 H, float NearCm);
  }
  ```

- [ ] **Step 1: Write the shared synthetic scene** — `Tests/ThermalTestScene.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ThermalFrameParams.h"
#include "Thermal/BandRadiance.h"
#include "Thermal/ThermalReference.h"

#include <limits>

/** Synthetic thermal scenes for CamSim.Thermal.Reference.* and CamSim.GPU.Thermal.* (ROADMAP 4A). Test code only. */
namespace CamSimThermalTest
{
	inline constexpr float NearCm  = 10.0f;
	inline constexpr float HFovDeg = 60.0f;

	inline const FBandRadiance& MwirBand()
	{
		static const FBandRadiance Band = [] { FBandRadiance B; B.Build(3.0, 5.0); return B; }();
		return Band;
	}

	/** Three classes (terrain 0, water 1, vehicle 2), every stencil -> vehicle + 8 K, camera 10 m above a flat sea. */
	inline FThermalFrameParams MakeParams(const FRotator& ViewRot, int32 W, int32 H)
	{
		FThermalFrameParams P;
		FMemory::Memcpy(P.LogLut, MwirBand().GetLogLut(), sizeof(P.LogLut));
		P.NumClasses = 3;
		P.ClassTempK[0] = 300.0f; P.ClassEmissivity[0] = 0.95f; P.ClassKFast[0] = 0.015f; P.ClassSAbsRef[0] = 400.0f;   // terrain
		P.ClassTempK[1] = 288.0f; P.ClassEmissivity[1] = 0.98f; P.ClassKFast[1] = 0.0f;   P.ClassSAbsRef[1] = 470.0f;   // water
		P.ClassTempK[2] = 295.0f; P.ClassEmissivity[2] = 0.90f; P.ClassKFast[2] = 0.04f;  P.ClassSAbsRef[2] = 350.0f;   // vehicle
		P.TerrainClass = 0;
		P.WaterClass = 1;
		for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S) { P.StencilClass[S] = 2; P.StencilOffsetK[S] = 8.0f; }
		P.ClipToTranslatedWorld = CamSimThermalRef::MakeClipToTranslatedWorld(ViewRot, HFovDeg, W, H, NearCm);
		P.Up = FVector3f(0.0f, 0.0f, 1.0f);
		P.bWater = 1;
		P.CamHeightCm = 1000.0f;
		P.SeaRadiusCm = 6.371e8f;
		P.WaterBandCm = 90.0f;
		P.TairK = 288.15f;
		P.SkyEpsZ = 0.7709f;
		P.Cloud = 0.0f;
		P.SkyHemiRadiance = MwirBand().Radiance(276.0f);
		P.BetaPerCm = 0.15f / 1e5f;
		P.KLum = 120.0f;
		P.EClampWm2 = 1200.0f;
		P.KFastScale = 1.0f;
		return P;
	}

	/** Device Z (reversed, infinite far) of the plane z = PlaneZCm (< 0, translated world, camera at the origin) seen through
	 *  output pixel centre (U, V); 0 when the ray misses it (sky). */
	inline float DeviceZForPlane(const FThermalFrameParams& P, const FRotator& ViewRot, float U, float V, float PlaneZCm)
	{
		const FVector4f H = P.ClipToTranslatedWorld.TransformFVector4(FVector4f(U * 2.0f - 1.0f, 1.0f - V * 2.0f, 1.0f, 1.0f));
		const FVector3f Dir = (FVector3f(H.X, H.Y, H.Z) / H.W).GetSafeNormal();
		if (!(Dir.Z < -1e-6f) || !(PlaneZCm < 0.0f)) return 0.0f;
		const float T = PlaneZCm / Dir.Z;
		const float ViewDepth = FVector3f::DotProduct(Dir * T, FVector3f(ViewRot.Vector()));
		return NearCm / ViewDepth;
	}

	/** Scene luminance of an unshadowed horizontal surface of albedo A receiving exactly the terrain class's reference flux:
	 *  (1 - A) E = S_abs,ref, Lum = A E K_lum / pi. */
	inline float SunlitLuminance(const FThermalFrameParams& P, float A)
	{
		return A * P.KLum * (P.ClassSAbsRef[0] / (1.0f - A)) / UE_PI;
	}

	inline bool InBox(float U, float V, float U0, float U1, float V0, float V1) { return U >= U0 && U < U1 && V >= V0 && V < V1; }

	struct FScene
	{
		int32 W = 0, H = 0, DW = 0, DH = 0;
		FRotator ViewRot = FRotator(-5.0, 0.0, 0.0);
		FThermalFrameParams P;
		TArray<FLinearColor> Color;    // W x H, absolute luminance units
		TArray<float> Depth, Custom;   // DW x DH, device Z
		TArray<uint8> Stencil;         // DW x DH
		TArray<FLinearColor> Base;     // DW x DH, linear base colour

		CamSimThermalRef::FImages Images(bool bWithBase) const
		{
			CamSimThermalRef::FImages I;
			I.W = W; I.H = H; I.DepthW = DW; I.DepthH = DH;
			I.SceneColor = &Color; I.SceneDepth = &Depth; I.CustomDepth = &Custom; I.Stencil = &Stencil;
			I.BaseColor = bWithBase ? &Base : nullptr;
			return I;
		}
	};

	/**
	 * Camera 10 m above the sea, pitched -5 deg: sky above the horizon; below it a sea-level plane on the left half (water)
	 * and a plane 3 m above the sea on the right (terrain). Entities: visible on terrain (stencil 3, darker base colour),
	 * occluded (4: custom depth twice as far as the scene) and visible on the water (5). A shadowed patch (1/4 of the sunlit
	 * luminance); one NaN and one -Inf colour pixel; one NaN and one +Inf depth texel.
	 */
	inline FScene MakeScene(int32 W, int32 H, int32 DW, int32 DH)
	{
		FScene S;
		S.W = W; S.H = H; S.DW = DW; S.DH = DH;
		S.P = MakeParams(S.ViewRot, W, H);
		S.Depth.Init(0.0f, DW * DH);
		S.Custom.Init(0.0f, DW * DH);
		S.Stencil.Init(0, DW * DH);
		S.Base.Init(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f), DW * DH);
		for (int32 Ty = 0; Ty < DH; ++Ty)
		{
			for (int32 Tx = 0; Tx < DW; ++Tx)
			{
				const int32 I = Ty * DW + Tx;
				const float U = (Tx + 0.5f) / DW, V = (Ty + 0.5f) / DH;
				const float Z = DeviceZForPlane(S.P, S.ViewRot, U, V, U < 0.5f ? -1000.0f : -700.0f);
				S.Depth[I] = Z;
				if (Z <= 0.0f) continue;
				if (InBox(U, V, 0.60f, 0.70f, 0.70f, 0.80f)) { S.Stencil[I] = 3; S.Custom[I] = Z; S.Base[I] = FLinearColor(0.1f, 0.1f, 0.1f, 1.0f); }
				if (InBox(U, V, 0.80f, 0.90f, 0.70f, 0.80f)) { S.Stencil[I] = 4; S.Custom[I] = Z * 0.5f; }
				if (InBox(U, V, 0.20f, 0.30f, 0.75f, 0.85f)) { S.Stencil[I] = 5; S.Custom[I] = Z; }
			}
		}
		S.Depth[(DH * 9 / 10) * DW + DW * 55 / 100] = std::numeric_limits<float>::quiet_NaN();
		S.Depth[(DH * 9 / 10) * DW + DW * 45 / 100] = std::numeric_limits<float>::infinity();
		const float Lit = SunlitLuminance(S.P, 0.2f);
		S.Color.Init(FLinearColor(Lit, Lit, Lit, 1.0f), W * H);
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				if (InBox((X + 0.5f) / W, (Y + 0.5f) / H, 0.60f, 0.75f, 0.55f, 0.65f))
				{
					S.Color[Y * W + X] = FLinearColor(0.25f * Lit, 0.25f * Lit, 0.25f * Lit, 1.0f);
				}
			}
		}
		S.Color[(H * 95 / 100) * W + W * 65 / 100] = FLinearColor(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 1.0f);
		S.Color[(H * 95 / 100) * W + W * 35 / 100] = FLinearColor(-std::numeric_limits<float>::infinity(), 0.0f, 0.0f, 1.0f);
		return S;
	}
}
```

- [ ] **Step 2: Write the failing tests** — `Tests/ThermalReferenceTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tests/ThermalTestScene.h"

#include <limits>

// CamSim.Thermal.Reference.*: the per-pixel CPU reference ThermalCS mirrors (ROADMAP 4A).

using namespace CamSimThermalTest;
using CamSimThermalRef::EPixelClass;
using CamSimThermalRef::FPixelResult;
using CamSimThermalRef::FPixelSample;

namespace
{
	/** A terrain sample (plane 7 m below the camera) at (U, V) with the sunlit luminance and base colour 0.2. */
	FPixelSample TerrainSample(const FThermalFrameParams& P, const FRotator& Rot, float U, float V)
	{
		FPixelSample S;
		S.U = U; S.V = V;
		S.DeviceZ = DeviceZForPlane(P, Rot, U, V, -700.0f);
		S.bHasBase = true;
		S.Base = FVector3f(0.2f);
		S.Color = FVector3f(SunlitLuminance(P, 0.2f));
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefClassesTest, "CamSim.Thermal.Reference.ClassificationOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefClassesTest::RunTest(const FString& Parameters)
{
	const FScene S = MakeScene(64, 36, 64, 36);
	const TArray<FPixelResult> R = CamSimThermalRef::Run(S.Images(true), S.P);
	if (!TestEqual(TEXT("one result per pixel"), R.Num(), S.W * S.H)) return false;
	auto At = [&](float U, float V) -> const FPixelResult& { return R[FMath::FloorToInt32(V * S.H) * S.W + FMath::FloorToInt32(U * S.W)]; };
	TestTrue(TEXT("top row: sky"), At(0.50f, 0.02f).Class == EPixelClass::Sky);
	TestTrue(TEXT("visible entity on terrain"), At(0.65f, 0.75f).Class == EPixelClass::Entity);
	TestTrue(TEXT("entity: class temperature + offset + its own fast term"), At(0.65f, 0.75f).TempK > S.P.ClassTempK[2] + 8.0f);
	TestTrue(TEXT("occluded entity: the terrain in front"), At(0.85f, 0.75f).Class == EPixelClass::Terrain);
	TestTrue(TEXT("visible entity over water beats water"), At(0.25f, 0.80f).Class == EPixelClass::Entity);
	TestTrue(TEXT("left half below the horizon: water"), At(0.10f, 0.70f).Class == EPixelClass::Water);
	TestTrue(TEXT("right half: terrain"), At(0.95f, 0.60f).Class == EPixelClass::Terrain);
	TestNearlyEqual(TEXT("water temperature = class"), At(0.10f, 0.70f).TempK, S.P.ClassTempK[1], 1e-4f);
	TestTrue(TEXT("sky colder than terrain"), At(0.50f, 0.02f).Radiance < At(0.95f, 0.60f).Radiance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefShadowTest, "CamSim.Thermal.Reference.FastTermShadowCooler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefShadowTest::RunTest(const FString& Parameters)
{
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Rot, 64, 36);
	P.bWater = 0;
	FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
	const FPixelResult Sun = CamSimThermalRef::EvaluatePixel(P, S);
	TestNearlyEqual(TEXT("sunlit reference surface: fast term 0"), Sun.TempK, P.ClassTempK[0], 1e-3f);
	S.Color *= 0.25f;
	const FPixelResult Shade = CamSimThermalRef::EvaluatePixel(P, S);
	TestNearlyEqual(TEXT("shadow: k_fast (S_abs,pix - S_abs,ref)"), Shade.TempK - Sun.TempK, 0.015f * (0.25f * 400.0f - 400.0f), 1e-3f);
	TestTrue(TEXT("shadow radiance lower"), Shade.Radiance < Sun.Radiance);
	S.Color *= 6.0f;   // 1.5x the reference: a sun-facing slope
	TestTrue(TEXT("sun-facing slope warmer"), CamSimThermalRef::EvaluatePixel(P, S).TempK > Sun.TempK);
	S.bHasBase = false;
	TestNearlyEqual(TEXT("no base colour: no fast term"), CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[0], 1e-6f);
	S.bHasBase = true;
	P.KFastScale = 0.0f;
	TestNearlyEqual(TEXT("KFastScale 0 (fallback): no fast term"), CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[0], 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefPathTest, "CamSim.Thermal.Reference.PathTermFarIsAir",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefPathTest::RunTest(const FString& Parameters)
{
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Rot, 64, 36);
	P.bWater = 0;
	const FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
	const float BAir = CamSimThermalRef::LutRadiance(P, P.TairK);
	P.BetaPerCm = 0.05f;   // tau = exp(-70) over ~14 m
	TestNearlyEqual(TEXT("opaque path: B(T_air)"), CamSimThermalRef::EvaluatePixel(P, S).Radiance, BAir, BAir * 1e-6f);
	P.BetaPerCm = 0.0f;
	const float Eps = P.ClassEmissivity[0];
	const float Expected = Eps * CamSimThermalRef::LutRadiance(P, P.ClassTempK[0]) + (1.0f - Eps) * P.SkyHemiRadiance;
	TestNearlyEqual(TEXT("no path: eps B(T) + (1 - eps) L_sky,hemi"), CamSimThermalRef::EvaluatePixel(P, S).Radiance, Expected, Expected * 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefNaNTest, "CamSim.Thermal.Reference.NaNHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefNaNTest::RunTest(const FString& Parameters)
{
	const FRotator Rot(-30.0, 0.0, 0.0);
	const FThermalFrameParams P = MakeParams(Rot, 64, 36);
	const float BAir = CamSimThermalRef::LutRadiance(P, P.TairK);
	const float NaN = std::numeric_limits<float>::quiet_NaN(), Inf = std::numeric_limits<float>::infinity();
	struct FCase { const TCHAR* Name; float Z; FVector3f C; };
	for (const FCase& C : { FCase{ TEXT("NaN colour"), 0.01f, FVector3f(NaN, 1.0f, 1.0f) }, FCase{ TEXT("-Inf colour"), 0.01f, FVector3f(1.0f, -Inf, 1.0f) },
		FCase{ TEXT("NaN depth"), NaN, FVector3f(1.0f) }, FCase{ TEXT("+Inf depth"), Inf, FVector3f(1.0f) } })
	{
		FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
		S.DeviceZ = C.Z;
		S.Color = C.C;
		const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
		TestTrue(*FString::Printf(TEXT("%s: invalid"), C.Name), R.Class == EPixelClass::Invalid);
		TestEqual(*FString::Printf(TEXT("%s: B(T_air)"), C.Name), R.Radiance, BAir);
	}
	const FScene Scene = MakeScene(64, 36, 64, 36);
	int32 NonFinite = 0;
	for (const FPixelResult& R : CamSimThermalRef::Run(Scene.Images(true), Scene.P)) NonFinite += FMath::IsFinite(R.Radiance) ? 0 : 1;
	TestEqual(TEXT("no NaN/Inf reaches the detector"), NonFinite, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefHorizonTest, "CamSim.Thermal.Reference.SkyBelowHorizonAndCameraAtSeaLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefHorizonTest::RunTest(const FString& Parameters)
{
	// Review focus 2. From altitude, sky pixels can look below the geometric horizon (sin el < 0).
	const FRotator Level(0.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Level, 64, 36);
	FPixelSample Sky;
	Sky.U = 0.5f; Sky.V = 0.9f; Sky.DeviceZ = 0.0f;
	const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, Sky);
	TestTrue(TEXT("below-horizon sky ray: sky"), R.Class == EPixelClass::Sky);
	TestTrue(TEXT("finite"), FMath::IsFinite(R.TempK) && FMath::IsFinite(R.Radiance));
	TestNearlyEqual(TEXT("clamped to MinSinEl"), R.TempK, CamSimThermalRef::SkyTemperatureK(P, 0.05f), 1e-4f);
	TestTrue(TEXT("between the zenith sky and T_air"), R.TempK >= CamSimThermalRef::SkyTemperatureK(P, 1.0f) && R.TempK <= P.TairK + 1e-3f);
	// A deck camera 30 cm below the still sea surface (wave trough): water 50 cm below the camera.
	const FRotator Down(-30.0, 0.0, 0.0);
	P = MakeParams(Down, 64, 36);
	P.CamHeightCm = -30.0f;
	FPixelSample W;
	W.U = 0.5f; W.V = 0.6f;
	W.DeviceZ = DeviceZForPlane(P, Down, W.U, W.V, -50.0f);
	const FPixelResult Rw = CamSimThermalRef::EvaluatePixel(P, W);
	TestTrue(TEXT("camera below the sea surface: water"), Rw.Class == EPixelClass::Water);
	TestTrue(TEXT("finite"), FMath::IsFinite(Rw.Radiance));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefExtremesTest, "CamSim.Thermal.Reference.FastTermExtremesFinite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefExtremesTest::RunTest(const FString& Parameters)
{
	// Review focus 5.
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Rot, 64, 36);
	P.bWater = 0;
	FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
	auto T = [&]() { return CamSimThermalRef::EvaluatePixel(P, S); };

	S.Base = FVector3f(0.0f);   // black paint: BaseLum clamps at 0.03, E at EClamp
	TestNearlyEqual(TEXT("black base colour: E clamped"), T().TempK, 300.0f + 0.015f * (1.0f * 1200.0f - 400.0f), 1e-3f);
	S.Base = FVector3f(0.2f);
	S.Color = FVector3f(1e30f);  // emissive / specular highlight
	TestNearlyEqual(TEXT("highlight: E clamped"), T().TempK, 300.0f + 0.015f * (0.8f * 1200.0f - 400.0f), 1e-3f);
	S.Color = FVector3f(-5.0f);  // negative scene colour
	TestNearlyEqual(TEXT("negative colour: E = 0"), T().TempK, 300.0f + 0.015f * (0.0f - 400.0f), 1e-3f);
	S.Color = FVector3f(SunlitLuminance(P, 0.2f));
	P.KLum = 0.0f;
	TestTrue(TEXT("K_lum 0: finite"), FMath::IsFinite(T().TempK) && FMath::IsFinite(T().Radiance));
	P.KLum = 120.0f;
	P.EClampWm2 = 0.0f;          // night: S = 0
	P.ClassSAbsRef[0] = 0.0f;
	TestEqual(TEXT("night: fast term exactly 0"), T().TempK, P.ClassTempK[0]);
	P.BetaPerCm = 0.0f;
	S.Stencil = 7; S.CustomZ = S.DeviceZ; P.StencilOffsetK[7] = 900.0f;   // past the LUT
	const float B1000 = FMath::Exp(P.LogLut[FThermalFrameParams::LutSize - 1]);
	TestNearlyEqual(TEXT("T > 1000 K saturates at B(1000 K)"), T().Radiance,
		P.ClassEmissivity[2] * B1000 + (1.0f - P.ClassEmissivity[2]) * P.SkyHemiRadiance, B1000 * 1e-5f);
	S.Stencil = 0;
	P.ClassTempK[0] = 100.0f;
	const float B150 = FMath::Exp(P.LogLut[0]);
	TestNearlyEqual(TEXT("T < 150 K clamps at B(150 K)"), T().Radiance,
		P.ClassEmissivity[0] * B150 + (1.0f - P.ClassEmissivity[0]) * P.SkyHemiRadiance, P.SkyHemiRadiance * 1e-5f);
	P.NumClasses = 1;
	S.Stencil = 9; S.CustomZ = S.DeviceZ;   // stencil table says class 2, but only one class exists
	TestTrue(TEXT("class index clamped below NumClasses"), FMath::IsFinite(T().Radiance) && T().TempK == P.ClassTempK[0] + P.StencilOffsetK[9]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefMappingTest, "CamSim.Thermal.Reference.DepthResolutionMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefMappingTest::RunTest(const FString& Parameters)
{
	// Output 8 x 4, depth (render resolution) 4 x 2: output pixel (x, y) reads texel (floor(U * 4), floor(V * 2)).
	const FRotator Rot(-30.0, 0.0, 0.0);
	FScene S;
	S.W = 8; S.H = 4; S.DW = 4; S.DH = 2;
	S.P = MakeParams(Rot, S.W, S.H);
	S.P.bWater = 0;
	for (int32 Ty = 0; Ty < S.DH; ++Ty)
		for (int32 Tx = 0; Tx < S.DW; ++Tx) S.Depth.Add(DeviceZForPlane(S.P, Rot, (Tx + 0.5f) / S.DW, (Ty + 0.5f) / S.DH, -700.0f));
	S.Custom.Init(0.0f, 8);
	S.Stencil.Init(0, 8);
	S.Base.Init(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f), 8);
	S.Stencil[1 * 4 + 3] = 3;
	S.Custom[1 * 4 + 3] = S.Depth[1 * 4 + 3];
	S.Color.Init(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f), 32);
	const TArray<FPixelResult> R = CamSimThermalRef::Run(S.Images(true), S.P);
	for (const FIntPoint Px : { FIntPoint(6, 2), FIntPoint(7, 2), FIntPoint(6, 3), FIntPoint(7, 3) })
	{
		TestTrue(*FString::Printf(TEXT("pixel (%d, %d) reads texel (3, 1)"), Px.X, Px.Y), R[Px.Y * 8 + Px.X].Class == EPixelClass::Entity);
	}
	TestTrue(TEXT("pixel (5, 3) reads texel (2, 1)"), R[3 * 8 + 5].Class == EPixelClass::Terrain);
	TestTrue(TEXT("pixel (6, 1) reads texel (3, 0)"), R[1 * 8 + 6].Class == EPixelClass::Terrain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefSrgbTest, "CamSim.Thermal.Reference.SrgbBaseColorDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefSrgbTest::RunTest(const FString& Parameters)
{
	TestNearlyEqual(TEXT("sRGB 0.5 -> linear"), CamSimThermalRef::SrgbToLinear(0.5f), 0.214041f, 1e-5f);
	TestNearlyEqual(TEXT("sRGB toe"), CamSimThermalRef::SrgbToLinear(0.02f), 0.02f / 12.92f, 1e-7f);
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Rot, 64, 36);
	P.bWater = 0;
	FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
	S.Base = FVector3f(0.5f);                                 // encoded value of linear 0.214
	S.Color = FVector3f(SunlitLuminance(P, 0.214041f));
	P.bBaseColorSrgb = 1;
	const float Decoded = CamSimThermalRef::EvaluatePixel(P, S).TempK;
	P.bBaseColorSrgb = 0;
	const float Raw = CamSimThermalRef::EvaluatePixel(P, S).TempK;
	// Decoded, the base colour (0.214) is the albedo the luminance was made with: the reference surface, fast term ~0.
	TestTrue(TEXT("decoded: the reference surface"), FMath::Abs(Decoded - P.ClassTempK[0]) < 0.01f);
	TestTrue(TEXT("raw (undecoded) value: far off"), FMath::Abs(Raw - P.ClassTempK[0]) > 2.0f);
	return true;
}
```

- [ ] **Step 3: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/ThermalReference.h` not found.

- [ ] **Step 4: Complete `FThermalFrameParams`.** Replace the struct in `Source/CamSimShaders/Public/ThermalFrameParams.h` (keep `ThermalLutRadiance` below it unchanged):

```cpp
/**
 * Everything one frame of ThermalCS needs besides its input textures (ROADMAP 4A). Filled on the game thread by
 * FThermalFrameBuilder (CamSimTest/Thermal); ClipToTranslatedWorld is set on the render thread from the view.
 * CamSimThermalRef::EvaluatePixel (CamSimTest/Thermal/ThermalReference.h) documents the per-pixel maths.
 */
struct FThermalFrameParams
{
	// In-band radiance LUT: LogLut[i] = ln B(T_i), T_i = LutMinK + i / LutScale (FBandRadiance::Build).
	static constexpr int32 LutSize     = 1024;
	static constexpr float LutMinK     = 150.0f;
	static constexpr float LutMaxK     = 1000.0f;   // upper range left for 4C exhausts
	static constexpr float LutScale    = static_cast<float>(LutSize - 1) / (LutMaxK - LutMinK);
	static constexpr int32 MaxClasses  = 32;
	static constexpr int32 NumStencils = 256;

	float LogLut[LutSize] = {};

	// Classes (index < NumClasses)
	uint32 NumClasses = 1;
	float  ClassTempK[MaxClasses]      = {};   // T_class(t), K
	float  ClassEmissivity[MaxClasses] = {};
	float  ClassKFast[MaxClasses]      = {};   // K per W m^-2
	float  ClassSAbsRef[MaxClasses]    = {};   // (1 - a) S_clear(t) cloud factor, W m^-2: what the class model assumed
	uint32 TerrainClass = 0;
	uint32 WaterClass   = 1;

	// Entities: custom-stencil value -> class + offset (stencil 0 = untagged)
	uint8 StencilClass[NumStencils]   = {};
	float StencilOffsetK[NumStencils] = {};
	float EntityDepthRatio = 0.99f;            // visible when custom depth >= scene depth * ratio (reversed Z: within 1 %)

	// Geometry: translated world (cm), the camera at the origin
	FMatrix44f ClipToTranslatedWorld = FMatrix44f::Identity;   // (NDC x, NDC y, device Z, 1) -> translated world, row vector
	FVector3f  Up = FVector3f(0.0f, 0.0f, 1.0f);               // geodetic up at the camera
	uint32 bWater      = 0;                    // 0: no sea surface (ocean off) -> no water class
	float  CamHeightCm = 0.0f;                 // camera height above the still sea surface at its nadir
	float  SeaRadiusCm = 6.371e8f;             // Gaussian radius of curvature at the camera
	float  WaterBandCm = 50.0f;                // water below this height above the sea (0.5 m + max wave amplitude)

	// Sky and atmosphere
	float TairK           = 288.15f;           // air temperature now
	float SkyEpsZ         = 0.77f;             // clear-sky zenith emissivity (T_clear / T_air)^4
	float Cloud           = 0.0f;
	float SkyHemiRadiance = 0.0f;              // view-factor average in-band sky radiance (reflected by 1 - eps)
	float BetaPerCm       = 0.0f;              // path extinction per cm

	// Solar fast term
	float  KLum           = 1.0f;              // lux per W m^-2: E_pix = pi Lum / (max(BaseLum, 0.03) K_lum)
	float  EClampWm2      = 0.0f;              // E_pix clamp: 1.5 S_clear(t)
	float  KFastScale     = 1.0f;              // 0: fast term off (no base colour / no sun light: Task 1 fallback)
	uint32 bBaseColorSrgb = 0;                 // 1: GBufferC values are sRGB-encoded (Task 1 SRGB_ENCODED)

	float InputScale = 1.0f;                   // multiplies scene colour; tests only (runtime also multiplies View.OneOverPreExposure)
};
```

- [ ] **Step 5: Implement the reference.** `Source/CamSimTest/Thermal/ThermalReference.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ThermalFrameParams.h"

/**
 * CPU reference of ThermalCS (ROADMAP 4A). Shaders/Private/CamSimThermalCommon.ush EvaluatePixel mirrors
 * EvaluatePixel below expression for expression; CamSim.GPU.Thermal.MatchesCpu holds them to 1e-4 relative.
 * Per output pixel (in order):
 *  0. non-finite scene colour or depth -> B(T_air) (Invalid)
 *  1. sky: device Z <= 0 -> B(T_sky(el)), el from the view ray, T_sky = T_air ((1-c) eps(el) + c)^(1/4)
 *  2. entity: stencil s > 0 and custom Z >= scene Z * EntityDepthRatio -> class/offset from the stencil table
 *  3. water: height above the sea h = H_cam + v + d^2 / 2R < WaterBandCm (v = P.up, d^2 = |P|^2 - v^2)
 *  4. terrain: TerrainClass
 *  T = T_class + offset [+ KFastScale k_fast ((1 - a_pix) E_pix - S_abs,ref)], a_pix = BaseLum,
 *  E_pix = clamp(pi Lum(colour) / (max(BaseLum, 0.03) max(K_lum, 1e-6)), 0, EClamp)
 *  L = tau (eps B(T) + (1 - eps) L_sky,hemi) + (1 - tau) B(T_air), tau = exp(-beta range)
 */
namespace CamSimThermalRef
{
	enum class EPixelClass : uint8 { Sky, Entity, Water, Terrain, Invalid };

	/** What ThermalCS reads for one output pixel. */
	struct FPixelSample
	{
		float U = 0.5f, V = 0.5f;                       // output pixel centre / output size
		float DeviceZ = 0.0f;                           // scene depth, reversed (1 near, 0 far)
		float CustomZ = 0.0f;                           // custom depth of tagged entities
		uint32 Stencil = 0;                             // custom stencil (low 8 bits used)
		FVector3f Color = FVector3f::ZeroVector;        // scene colour * InputScale (absolute luminance units)
		FVector3f Base = FVector3f::ZeroVector;         // GBuffer base colour
		bool bHasBase = false;                          // the base colour texture is bound
	};

	struct FPixelResult
	{
		EPixelClass Class = EPixelClass::Terrain;
		float TempK = 0.0f;
		float Radiance = 0.0f;                          // W m^-2 sr^-1 in band
	};

	/** Row-major view-rect contents: colour W x H (output resolution), depth/stencil/base DepthW x DepthH (render resolution). */
	struct FImages
	{
		int32 W = 0, H = 0, DepthW = 0, DepthH = 0;
		const TArray<FLinearColor>* SceneColor  = nullptr;
		const TArray<float>*        SceneDepth  = nullptr;
		const TArray<float>*        CustomDepth = nullptr;
		const TArray<uint8>*        Stencil     = nullptr;
		const TArray<FLinearColor>* BaseColor   = nullptr;   // null: not bound (fast term off)
	};

	float LutRadiance(const FThermalFrameParams& P, float TK);
	float SkyTemperatureK(const FThermalFrameParams& P, float SinEl);
	float SrgbToLinear(float C);
	FPixelResult EvaluatePixel(const FThermalFrameParams& P, const FPixelSample& S);
	/** Every output pixel; pixel (x, y) reads texel (floor(U DepthW), floor(V DepthH)) of the depth-resolution images. */
	TArray<FPixelResult> Run(const FImages& In, const FThermalFrameParams& P);
	/** ClipToTranslatedWorld of a camera at the origin with this rotation and horizontal FOV (UE's reversed-Z infinite projection). */
	FMatrix44f MakeClipToTranslatedWorld(const FRotator& ViewRotation, float HFovDeg, int32 W, int32 H, float NearCm);
}
```

`Source/CamSimTest/Thermal/ThermalReference.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalReference.h"

namespace CamSimThermalRef
{
	namespace
	{
		constexpr float PiF = 3.14159265f;   // THERMAL_PI in CamSimThermalCommon.ush

		float Lum709(const FVector3f& C) { return 0.2126f * C.X + 0.7152f * C.Y + 0.0722f * C.Z; }

		FVector3f ClipToWorld(const FThermalFrameParams& P, float Nx, float Ny, float Z)
		{
			const FVector4f H = P.ClipToTranslatedWorld.TransformFVector4(FVector4f(Nx, Ny, Z, 1.0f));
			return FVector3f(H.X / H.W, H.Y / H.W, H.Z / H.W);
		}
	}

	float LutRadiance(const FThermalFrameParams& P, float TK) { return ThermalLutRadiance(P.LogLut, TK); }

	float SkyTemperatureK(const FThermalFrameParams& P, float SinEl)
	{
		const float S = FMath::Max(SinEl, 0.05f);
		const float Eps = 1.0f - FMath::Pow(1.0f - P.SkyEpsZ, 1.0f / S);
		const float Ratio = (1.0f - P.Cloud) * Eps + P.Cloud;
		return P.TairK * FMath::Sqrt(FMath::Sqrt(Ratio));
	}

	float SrgbToLinear(float C)
	{
		return C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
	}

	FPixelResult EvaluatePixel(const FThermalFrameParams& P, const FPixelSample& S)
	{
		FPixelResult R;
		const float BAir = LutRadiance(P, P.TairK);
		if (!FMath::IsFinite(S.DeviceZ) || !FMath::IsFinite(S.Color.X) || !FMath::IsFinite(S.Color.Y) || !FMath::IsFinite(S.Color.Z))
		{
			R.Class = EPixelClass::Invalid;
			R.TempK = P.TairK;
			R.Radiance = BAir;
			return R;
		}
		const float Nx = S.U * 2.0f - 1.0f, Ny = 1.0f - S.V * 2.0f;
		if (S.DeviceZ <= 0.0f)
		{
			const FVector3f Pn = ClipToWorld(P, Nx, Ny, 1.0f);
			const float Len = FMath::Sqrt(FVector3f::DotProduct(Pn, Pn));
			const float SinEl = (Len > 0.0f) ? FVector3f::DotProduct(Pn, P.Up) / Len : 1.0f;
			R.Class = EPixelClass::Sky;
			R.TempK = SkyTemperatureK(P, SinEl);
			R.Radiance = LutRadiance(P, R.TempK);
			return R;
		}
		const FVector3f Pw = ClipToWorld(P, Nx, Ny, S.DeviceZ);
		const float Range = FMath::Sqrt(FVector3f::DotProduct(Pw, Pw));
		uint32 Class = P.TerrainClass;
		float Offset = 0.0f;
		R.Class = EPixelClass::Terrain;
		const uint32 Stencil = S.Stencil & 0xFFu;
		if (Stencil > 0u && S.CustomZ >= S.DeviceZ * P.EntityDepthRatio)
		{
			Class = P.StencilClass[Stencil];
			Offset = P.StencilOffsetK[Stencil];
			R.Class = EPixelClass::Entity;
		}
		else if (P.bWater != 0u)
		{
			const float Vert = FVector3f::DotProduct(Pw, P.Up);
			const float D2 = FMath::Max(Range * Range - Vert * Vert, 0.0f);
			const float Height = P.CamHeightCm + Vert + D2 / (2.0f * P.SeaRadiusCm);
			if (Height < P.WaterBandCm)
			{
				Class = P.WaterClass;
				R.Class = EPixelClass::Water;
			}
		}
		Class = FMath::Min(Class, FMath::Max(P.NumClasses, 1u) - 1u);
		float T = P.ClassTempK[Class] + Offset;
		if (S.bHasBase && P.KFastScale > 0.0f)
		{
			const FVector3f Base = (P.bBaseColorSrgb != 0u)
				? FVector3f(SrgbToLinear(S.Base.X), SrgbToLinear(S.Base.Y), SrgbToLinear(S.Base.Z)) : S.Base;
			const float BaseLum = Lum709(Base);
			const float E = FMath::Clamp(PiF * Lum709(S.Color) / (FMath::Max(BaseLum, 0.03f) * FMath::Max(P.KLum, 1e-6f)), 0.0f, P.EClampWm2);
			const float SAbs = (1.0f - FMath::Clamp(BaseLum, 0.0f, 1.0f)) * E;
			T += P.KFastScale * P.ClassKFast[Class] * (SAbs - P.ClassSAbsRef[Class]);
		}
		const float Eps = P.ClassEmissivity[Class];
		const float LSurf = Eps * LutRadiance(P, T) + (1.0f - Eps) * P.SkyHemiRadiance;
		const float Tau = FMath::Exp(-P.BetaPerCm * Range);
		R.TempK = T;
		R.Radiance = Tau * LSurf + (1.0f - Tau) * BAir;
		return R;
	}

	TArray<FPixelResult> Run(const FImages& In, const FThermalFrameParams& P)
	{
		check(In.SceneColor && In.SceneDepth && In.CustomDepth && In.Stencil);
		check(In.SceneColor->Num() == In.W * In.H);
		check(In.SceneDepth->Num() == In.DepthW * In.DepthH && In.CustomDepth->Num() == In.DepthW * In.DepthH && In.Stencil->Num() == In.DepthW * In.DepthH);
		check(!In.BaseColor || In.BaseColor->Num() == In.DepthW * In.DepthH);
		TArray<FPixelResult> Out;
		Out.SetNum(In.W * In.H);
		for (int32 Y = 0; Y < In.H; ++Y)
		{
			for (int32 X = 0; X < In.W; ++X)
			{
				FPixelSample S;
				S.U = (static_cast<float>(X) + 0.5f) / static_cast<float>(In.W);
				S.V = (static_cast<float>(Y) + 0.5f) / static_cast<float>(In.H);
				const int32 Tx = FMath::Clamp(FMath::FloorToInt32(S.U * static_cast<float>(In.DepthW)), 0, In.DepthW - 1);
				const int32 Ty = FMath::Clamp(FMath::FloorToInt32(S.V * static_cast<float>(In.DepthH)), 0, In.DepthH - 1);
				const int32 Ti = Ty * In.DepthW + Tx;
				S.DeviceZ = (*In.SceneDepth)[Ti];
				S.CustomZ = (*In.CustomDepth)[Ti];
				S.Stencil = (*In.Stencil)[Ti];
				const FLinearColor& C = (*In.SceneColor)[Y * In.W + X];
				S.Color = FVector3f(C.R, C.G, C.B) * P.InputScale;
				if (In.BaseColor)
				{
					const FLinearColor& B = (*In.BaseColor)[Ti];
					S.Base = FVector3f(B.R, B.G, B.B);
					S.bHasBase = true;
				}
				Out[Y * In.W + X] = EvaluatePixel(P, S);
			}
		}
		return Out;
	}

	FMatrix44f MakeClipToTranslatedWorld(const FRotator& ViewRotation, float HFovDeg, int32 W, int32 H, float NearCm)
	{
		// View space: X right, Y up, Z forward (the axis swap UE applies after the inverse view rotation).
		const FMatrix ViewRot = FInverseRotationMatrix(ViewRotation) * FMatrix(
			FPlane(0.0, 0.0, 1.0, 0.0), FPlane(1.0, 0.0, 0.0, 0.0), FPlane(0.0, 1.0, 0.0, 0.0), FPlane(0.0, 0.0, 0.0, 1.0));
		const FMatrix Proj = FReversedZPerspectiveMatrix(FMath::DegreesToRadians(0.5 * HFovDeg), double(W), double(H), double(NearCm));
		return FMatrix44f((ViewRot * Proj).Inverse());
	}
}
```

- [ ] **Step 6: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Thermal.Reference && run_tests CamSim.Thermal.Planck` → Expected: `CamSim.Thermal.Reference` 8 succeeded 0 failed; Planck still 4 succeeded.

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalFrameParams.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalReference.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalReference.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalTestScene.h \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalReferenceTest.cpp
git commit -F - <<'EOF'
feat(thermal): FThermalFrameParams and the per-pixel CPU reference of ThermalCS

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 8: `ThermalCS` and `AddThermalPass`, held to the reference on the GPU

**Files:**
- Create: `unreal_project/CamSimTest/Shaders/Private/CamSimThermalCommon.ush`, `unreal_project/CamSimTest/Shaders/Private/CamSimThermal.usf`
- Create: `unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalPass.h`, `unreal_project/CamSimTest/Source/CamSimShaders/Private/ThermalPass.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalGpuTest.cpp`

**Interfaces:**
- Consumes: `FThermalFrameParams` (Task 7), `CamSimThermalRef::Run`, `CamSimThermalTest::MakeScene` (Task 7).
- Produces:
  ```cpp
  namespace CamSimThermalPass
  {
      inline constexpr bool bBaseColorAtTonemapper;   // from the Task 1 record
      inline constexpr bool bBaseColorSrgbEncoded;    // from the Task 1 record
  }
  struct FThermalPassInputs
  {
      FRDGTextureRef SceneColor; FIntRect SceneColorRect;
      FRDGTextureRef SceneDepth, CustomDepth; FRDGTextureSRVRef CustomStencil;
      FRDGTextureRef BaseColor;                 // null: fast term off
      FIntRect DepthViewRect;                   // tests; runtime takes the rect from the view
      FRHIUniformBuffer* ViewUniformBuffer;     // runtime: depth rect + View.OneOverPreExposure
  };
  CAMSIMSHADERS_API bool IsThermalPassSupported(FString& OutWhy);
  /** R32F radiance (W m^-2 sr^-1), extent = SceneColorRect.Size(), view rect at (0, 0). */
  CAMSIMSHADERS_API FRDGTextureRef AddThermalPass(FRDGBuilder& GraphBuilder, const FThermalPassInputs& In, const FThermalFrameParams& Params);
  ```

- [ ] **Step 1: Write the failing GPU test** — `Tests/ThermalGpuTest.cpp`:

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
#include "ThermalPass.h"
#include "Tests/ThermalTestScene.h"

// CamSim.GPU.Thermal.*: ThermalCS (CamSimThermal.usf) against CamSimThermalRef on synthetic textures — sky, entity
// visible/occluded/over water, water, terrain, shadow, NaN/Inf — within 1e-4 relative radiance (ROADMAP 4A).

namespace
{
	template <typename T>
	TArray<T> Embed(const TArray<T>& Src, int32 W, int32 H, FIntPoint Extent, FIntPoint Min, const T& Fill)
	{
		TArray<T> Out;
		Out.Init(Fill, Extent.X * Extent.Y);
		for (int32 Y = 0; Y < H; ++Y)
			for (int32 X = 0; X < W; ++X) Out[(Min.Y + Y) * Extent.X + Min.X + X] = Src[Y * W + X];
		return Out;
	}

	FTextureRHIRef UploadRgba(FRHICommandListImmediate& RHICmdList, const TArray<FLinearColor>& Texels, FIntPoint Ext, const TCHAR* Name)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(Name, Ext.X, Ext.Y, PF_A32B32G32R32F)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * sizeof(FLinearColor),
			reinterpret_cast<const uint8*>(Texels.GetData()));
		return Tex;
	}

	FTextureRHIRef UploadFloat(FRHICommandListImmediate& RHICmdList, const TArray<float>& Texels, FIntPoint Ext, const TCHAR* Name)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(Name, Ext.X, Ext.Y, PF_R32_FLOAT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * sizeof(float),
			reinterpret_cast<const uint8*>(Texels.GetData()));
		return Tex;
	}

	/** PF_R8G8B8A8_UINT with the value in every channel, so STENCIL_COMPONENT_SWIZZLE reads it whichever channel it names. */
	FTextureRHIRef UploadStencil(FRHICommandListImmediate& RHICmdList, const TArray<uint8>& Ids, FIntPoint Ext)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestThermalStencil"), Ext.X, Ext.Y, PF_R8G8B8A8_UINT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		TArray<uint8> Rgba;
		Rgba.SetNumUninitialized(Ids.Num() * 4);
		for (int32 I = 0; I < Ids.Num(); ++I) { for (int32 C = 0; C < 4; ++C) Rgba[I * 4 + C] = Ids[I]; }
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * 4, Rgba.GetData());
		return Tex;
	}

	struct FLayout { FIntPoint ColorExtent, ColorMin, DepthExtent, DepthMin; };

	/** Upload the scene into textures laid out per L (view rects offset inside larger textures), run AddThermalPass, read back. */
	TArray<float> RunThermalOnGpu(const CamSimThermalTest::FScene& S, const FLayout& L, bool bBase)
	{
		const TArray<FLinearColor> Color = Embed(S.Color, S.W, S.H, L.ColorExtent, L.ColorMin, FLinearColor::Black);
		const TArray<float> Depth = Embed(S.Depth, S.DW, S.DH, L.DepthExtent, L.DepthMin, 0.0f);
		const TArray<float> Custom = Embed(S.Custom, S.DW, S.DH, L.DepthExtent, L.DepthMin, 0.0f);
		const TArray<uint8> Stencil = Embed(S.Stencil, S.DW, S.DH, L.DepthExtent, L.DepthMin, static_cast<uint8>(0));
		const TArray<FLinearColor> Base = Embed(S.Base, S.DW, S.DH, L.DepthExtent, L.DepthMin, FLinearColor::Black);
		TArray<float> Result;
		ENQUEUE_RENDER_COMMAND(CamSimThermalGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef ColorTex   = UploadRgba(RHICmdList, Color, L.ColorExtent, TEXT("CamSimTestThermalColor"));
			FTextureRHIRef DepthTex   = UploadFloat(RHICmdList, Depth, L.DepthExtent, TEXT("CamSimTestThermalDepth"));
			FTextureRHIRef CustomTex  = UploadFloat(RHICmdList, Custom, L.DepthExtent, TEXT("CamSimTestThermalCustom"));
			FTextureRHIRef StencilTex = UploadStencil(RHICmdList, Stencil, L.DepthExtent);
			FTextureRHIRef BaseTex    = UploadRgba(RHICmdList, Base, L.DepthExtent, TEXT("CamSimTestThermalBase"));
			FRHIGPUTextureReadback Rb(TEXT("CamSimTestThermalRadiance"));
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FThermalPassInputs In;
				In.SceneColor     = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(ColorTex, TEXT("CamSimTestThermalColor")));
				In.SceneColorRect = FIntRect(L.ColorMin, L.ColorMin + FIntPoint(S.W, S.H));
				In.SceneDepth     = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DepthTex, TEXT("CamSimTestThermalDepth")));
				In.CustomDepth    = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(CustomTex, TEXT("CamSimTestThermalCustom")));
				const FRDGTextureRef StencilRdg = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(StencilTex, TEXT("CamSimTestThermalStencil")));
				In.CustomStencil  = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(StencilRdg));
				In.BaseColor      = bBase ? GraphBuilder.RegisterExternalTexture(CreateRenderTarget(BaseTex, TEXT("CamSimTestThermalBase"))) : nullptr;
				In.DepthViewRect  = FIntRect(L.DepthMin, L.DepthMin + FIntPoint(S.DW, S.DH));
				const FRDGTextureRef Radiance = AddThermalPass(GraphBuilder, In, S.P);
				AddEnqueueCopyPass(GraphBuilder, &Rb, Radiance);
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!Rb.IsReady()) return;
			int32 Pitch = 0;
			const float* Data = static_cast<const float*>(Rb.Lock(Pitch));
			Result.SetNumUninitialized(S.W * S.H);
			for (int32 Y = 0; Y < S.H; ++Y) FMemory::Memcpy(&Result[Y * S.W], Data + static_cast<int64>(Y) * Pitch, S.W * sizeof(float));
			Rb.Unlock();
		});
		FlushRenderingCommands();
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalGpuMatchesCpuTest, "CamSim.GPU.Thermal.MatchesCpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalGpuMatchesCpuTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
	struct FCase { const TCHAR* Name; int32 W, H, DW, DH; FIntPoint ColorPad, DepthPad; bool bBase, bSrgb; };
	const FCase Cases[] = {
		{ TEXT("same size"),                    64, 36, 64, 36, FIntPoint(0, 0), FIntPoint(0, 0), true,  false },
		{ TEXT("half-res depth, offset rects"), 64, 36, 32, 18, FIntPoint(3, 2), FIntPoint(4, 3), true,  false },
		{ TEXT("no base colour"),               64, 36, 64, 36, FIntPoint(0, 0), FIntPoint(0, 0), false, false },
		{ TEXT("sRGB-encoded base colour"),     64, 36, 64, 36, FIntPoint(0, 0), FIntPoint(0, 0), true,  true  },
	};
	for (const FCase& C : Cases)
	{
		CamSimThermalTest::FScene S = CamSimThermalTest::MakeScene(C.W, C.H, C.DW, C.DH);
		S.P.bBaseColorSrgb = C.bSrgb ? 1u : 0u;
		const TArray<CamSimThermalRef::FPixelResult> Ref = CamSimThermalRef::Run(S.Images(C.bBase), S.P);
		FLayout L;
		L.ColorMin = C.ColorPad;
		L.ColorExtent = FIntPoint(C.W, C.H) + C.ColorPad * 2;
		L.DepthMin = C.DepthPad;
		L.DepthExtent = FIntPoint(C.DW, C.DH) + C.DepthPad * 2;
		const TArray<float> Gpu = RunThermalOnGpu(S, L, C.bBase);
		if (!TestEqual(*FString::Printf(TEXT("%s: readback"), C.Name), Gpu.Num(), C.W * C.H)) continue;
		int32 Bad = 0, WorstI = 0;
		float WorstRel = 0.0f;
		TSet<uint8> Classes;
		for (int32 I = 0; I < Gpu.Num(); ++I)
		{
			const float R = Ref[I].Radiance;
			const float Rel = FMath::Abs(Gpu[I] - R) / FMath::Max(FMath::Abs(R), 1e-6f);
			Classes.Add(static_cast<uint8>(Ref[I].Class));
			if (!(Rel <= 1e-4f)) ++Bad;
			if (!(Rel <= WorstRel)) { WorstRel = Rel; WorstI = I; }
		}
		TestEqual(*FString::Printf(TEXT("%s: pixels beyond 1e-4 relative (worst %.3g at (%d, %d), class %d, gpu %.6g ref %.6g)"),
			C.Name, WorstRel, WorstI % C.W, WorstI / C.W, static_cast<int32>(Ref[WorstI].Class), Gpu[WorstI], Ref[WorstI].Radiance), Bad, 0);
		TestEqual(*FString::Printf(TEXT("%s: every class present"), C.Name), Classes.Num(), 5);
	}
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `ThermalPass.h` not found.

- [ ] **Step 3: Implement the shader.** `Shaders/Private/CamSimThermalCommon.ush`:

```hlsl
// Copyright CamSim Contributors. All Rights Reserved.
// Thermal core (ROADMAP 4A): ThermalCS's per-pixel maths and parameters. CPU twin: CamSimThermalRef::EvaluatePixel
// (CamSimTest/Thermal/ThermalReference.cpp), mirrored expression for expression (same order, same constants);
// CamSim.GPU.Thermal.MatchesCpu holds them to 1e-4 relative radiance. Plain float maths: no atomics, no wave intrinsics.

#pragma once

#define THERMAL_LUT_SIZE 1024
#define THERMAL_PI 3.14159265

float4   LogLut[THERMAL_LUT_SIZE / 4];   // ln B(T_i), T_i = LutMinK + i / LutScale, four per float4
float    LutMinK;
float    LutMaxK;
float    LutScale;
float4   ClassData[32];                  // x T_class (K), y emissivity, z k_fast, w S_abs,ref (W m^-2)
float4   StencilData[128];               // stencil 2k: (x class, y offset K); 2k + 1: (z class, w offset K)
uint     NumClasses;
uint     TerrainClass;
uint     WaterClass;
float    EntityDepthRatio;
float4x4 ClipToTranslatedWorld;          // (NDC x, NDC y, device Z, 1) -> translated world (row vector, reversed Z)
float3   Up;
uint     bWater;
float    CamHeightCm;
float    SeaRadiusCm;
float    WaterBandCm;
float    TairK;
float    SkyEpsZ;
float    Cloud;
float    SkyHemiRadiance;
float    BetaPerCm;
float    KLum;
float    EClampWm2;
float    KFastScale;
uint     bBaseColorSrgb;
uint     UseBaseColor;                   // 0: no base colour bound (fast term off)

/** Non-finite on the bits: Metal fast-math may fold isnan/isinf or V != V away. */
bool IsNonFinite(float V) { return (asuint(V) & 0x7fffffffu) >= 0x7f800000u; }

float LutAt(uint I) { return LogLut[I >> 2u][I & 3u]; }

/** ThermalLutRadiance (ThermalFrameParams.h). */
float LutRadiance(float TK)
{
	float T = TK;
	if (!(T >= LutMinK)) T = LutMinK;
	if (T > LutMaxK) T = LutMaxK;
	const float X = (T - LutMinK) * LutScale;
	const uint I = min((uint)floor(X), (uint)(THERMAL_LUT_SIZE - 2));
	const float F = X - float(I);
	const float A = LutAt(I);
	return exp(A + (LutAt(I + 1u) - A) * F);
}

float SkyTemperatureK(float SinEl)
{
	const float S = max(SinEl, 0.05);
	const float Eps = 1.0 - pow(1.0 - SkyEpsZ, 1.0 / S);
	const float Ratio = (1.0 - Cloud) * Eps + Cloud;
	return TairK * sqrt(sqrt(Ratio));
}

float SrgbToLinear(float C) { return C <= 0.04045 ? C / 12.92 : pow((C + 0.055) / 1.055, 2.4); }

float Lum709(float3 C) { return 0.2126 * C.x + 0.7152 * C.y + 0.0722 * C.z; }

float3 ClipToWorld(float Nx, float Ny, float Z)
{
	const float4 H = mul(float4(Nx, Ny, Z, 1.0), ClipToTranslatedWorld);
	return float3(H.x / H.w, H.y / H.w, H.z / H.w);
}

struct FThermalSample
{
	float  U;
	float  V;
	float  DeviceZ;
	float  CustomZ;
	uint   Stencil;
	float3 Color;   // scene colour * scale (absolute luminance units)
	float3 Base;    // GBuffer base colour
};

/** CamSimThermalRef::EvaluatePixel: in-band radiance of one output pixel. */
float EvaluatePixel(FThermalSample S)
{
	const float BAir = LutRadiance(TairK);
	if (IsNonFinite(S.DeviceZ) || IsNonFinite(S.Color.x) || IsNonFinite(S.Color.y) || IsNonFinite(S.Color.z)) return BAir;
	const float Nx = S.U * 2.0 - 1.0, Ny = 1.0 - S.V * 2.0;
	if (S.DeviceZ <= 0.0)
	{
		const float3 Pn = ClipToWorld(Nx, Ny, 1.0);
		const float Len = sqrt(dot(Pn, Pn));
		const float SinEl = (Len > 0.0) ? dot(Pn, Up) / Len : 1.0;
		return LutRadiance(SkyTemperatureK(SinEl));
	}
	const float3 Pw = ClipToWorld(Nx, Ny, S.DeviceZ);
	const float Range = sqrt(dot(Pw, Pw));
	uint Class = TerrainClass;
	float Offset = 0.0;
	const uint Stencil = S.Stencil & 0xFFu;
	if (Stencil > 0u && S.CustomZ >= S.DeviceZ * EntityDepthRatio)
	{
		const float4 E = StencilData[Stencil >> 1u];
		const bool bOdd = (Stencil & 1u) != 0u;
		Class = (uint)(bOdd ? E.z : E.x);
		Offset = bOdd ? E.w : E.y;
	}
	else if (bWater != 0u)
	{
		const float Vert = dot(Pw, Up);
		const float D2 = max(Range * Range - Vert * Vert, 0.0);
		const float Height = CamHeightCm + Vert + D2 / (2.0 * SeaRadiusCm);
		if (Height < WaterBandCm) Class = WaterClass;
	}
	Class = min(Class, max(NumClasses, 1u) - 1u);
	const float4 Cd = ClassData[Class];
	float T = Cd.x + Offset;
	if (UseBaseColor != 0u && KFastScale > 0.0)
	{
		const float3 Base = (bBaseColorSrgb != 0u) ? float3(SrgbToLinear(S.Base.x), SrgbToLinear(S.Base.y), SrgbToLinear(S.Base.z)) : S.Base;
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

`Shaders/Private/CamSimThermal.usf`:

```hlsl
// Copyright CamSim Contributors. All Rights Reserved.
// Thermal core (ROADMAP 4A): ThermalCS — for every pixel of the scene-colour view rect, classify sky / entity / water /
// terrain from scene depth, custom depth + stencil and GBuffer base colour (render resolution, nearest texel), add the
// solar fast term from scene colour, and write in-band radiance (R32F). Maths: CamSimThermalCommon.ush.

#include "/Engine/Public/Platform.ush"
#if USE_VIEW
#include "/Engine/Private/Common.ush"   // View.ViewRectMin, View.ViewSizeAndInvSize, View.OneOverPreExposure
#endif
#include "CamSimThermalCommon.ush"

int2  OutSize;          // = scene colour view rect size
int2  ColorViewMin;
int2  DepthViewMin;     // tests (runtime: View.ViewRectMin)
int2  DepthViewSize;    // tests (runtime: View.ViewSizeAndInvSize.xy)
float InputScale;       // tests; runtime multiplies View.OneOverPreExposure in as well
Texture2D<float4>  SceneColor;
Texture2D<float>   SceneDepth;
Texture2D<float>   CustomDepth;
Texture2D<uint2>   CustomStencil;   // FSceneTextureUniformParameters::CustomStencilTexture's type
Texture2D<float4>  BaseColor;       // GBufferC (black dummy when UseBaseColor == 0)
RWTexture2D<float> OutRadiance;

void DepthRect(out int2 Min, out int2 Size)
{
#if USE_VIEW
	Min = int2(View.ViewRectMin.xy);
	Size = int2(View.ViewSizeAndInvSize.xy);
#else
	Min = DepthViewMin;
	Size = DepthViewSize;
#endif
}

float ColorScale()
{
	float Scale = InputScale;
#if USE_VIEW
	Scale *= View.OneOverPreExposure;
#endif
	return Scale;
}

[numthreads(8, 8, 1)]
void ThermalCS(uint3 Id : SV_DispatchThreadID)
{
	const int2 P = int2(Id.xy);
	if (any(P >= OutSize)) return;
	int2 DMin, DSize;
	DepthRect(DMin, DSize);
	FThermalSample S;
	S.U = (float(P.x) + 0.5) / float(OutSize.x);
	S.V = (float(P.y) + 0.5) / float(OutSize.y);
	const int2 T = DMin + clamp(int2(floor(float2(S.U, S.V) * float2(DSize))), int2(0, 0), DSize - 1);
	S.DeviceZ = SceneDepth.Load(int3(T, 0));
	S.CustomZ = CustomDepth.Load(int3(T, 0));
	S.Stencil = CustomStencil.Load(int3(T, 0)) STENCIL_COMPONENT_SWIZZLE & 0xFFu;
	S.Color = SceneColor.Load(int3(ColorViewMin + P, 0)).rgb * ColorScale();
	S.Base = BaseColor.Load(int3(T, 0)).rgb;
	OutRadiance[P] = EvaluatePixel(S);
}
```

- [ ] **Step 4: Implement the pass.** `Source/CamSimShaders/Public/ThermalPass.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"
#include "ThermalFrameParams.h"

class FRHIUniformBuffer;

namespace CamSimThermalPass
{
	/**
	 * ROADMAP 4A Task 1 spike, recorded in ROADMAP.md ("4A Thermal core"): GBufferC holds the material base colour at
	 * ReplacingTonemapper with r.Substrate=True and r.Substrate.ProjectGBufferFormat=0. false = FALLBACK: the fast solar
	 * term is off (FThermalFrameParams::KFastScale = 0) and UCamSimSubsystem logs one warning.
	 * Record -> values: LINEAR -> true/false; SRGB_ENCODED -> true/true; FALLBACK -> false/false.
	 */
	inline constexpr bool bBaseColorAtTonemapper = true;
	/** Same spike: GBufferC values are sRGB-encoded in a non-sRGB format; ThermalCS decodes them (FThermalFrameParams::bBaseColorSrgb). */
	inline constexpr bool bBaseColorSrgbEncoded = false;
}

/** Inputs of ThermalCS (ROADMAP 4A). */
struct FThermalPassInputs
{
	FRDGTextureRef    SceneColor    = nullptr;   // tonemapper input (HDR; pre-exposed unless ViewUniformBuffer is null)
	FIntRect          SceneColorRect;            // its view rect; the radiance texture is SceneColorRect.Size()
	FRDGTextureRef    SceneDepth    = nullptr;   // device Z (reversed: 1 near, 0 far)
	FRDGTextureRef    CustomDepth   = nullptr;   // device Z of the tagged entities
	FRDGTextureSRVRef CustomStencil = nullptr;   // uint; read via STENCIL_COMPONENT_SWIZZLE
	FRDGTextureRef    BaseColor     = nullptr;   // GBufferC; null: fast term off
	FIntRect          DepthViewRect;             // region of the depth/stencil/base textures (ignored when ViewUniformBuffer is set)
	FRHIUniformBuffer* ViewUniformBuffer = nullptr;   // runtime: rect = View.ViewRectMin/ViewSizeAndInvSize, colour * View.OneOverPreExposure
};

/** Whether ThermalCS (both permutations) is in the global shader map. Game thread, after RHI init. */
CAMSIMSHADERS_API bool IsThermalPassSupported(FString& OutWhy);

/** ThermalCS: R32F in-band radiance (W m^-2 sr^-1), extent SceneColorRect.Size(), its view rect at (0, 0). See CamSimThermalRef. */
CAMSIMSHADERS_API FRDGTextureRef AddThermalPass(FRDGBuilder& GraphBuilder, const FThermalPassInputs& In, const FThermalFrameParams& Params);
```

`Source/CamSimShaders/Private/ThermalPass.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "ThermalPass.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "SceneView.h"
#include "SystemTextures.h"

static_assert(FThermalFrameParams::LutSize == 1024, "LogLut[256] below and THERMAL_LUT_SIZE in CamSimThermalCommon.ush");
static_assert(FThermalFrameParams::MaxClasses == 32, "ClassData[32] below and in CamSimThermalCommon.ush");
static_assert(FThermalFrameParams::NumStencils == 256, "StencilData[128] below and in CamSimThermalCommon.ush");

/** ThermalCS parameters (CamSimThermal.usf + CamSimThermalCommon.ush). */
BEGIN_SHADER_PARAMETER_STRUCT(FCamSimThermalParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER(FIntPoint, OutSize)
	SHADER_PARAMETER(FIntPoint, ColorViewMin)
	SHADER_PARAMETER(FIntPoint, DepthViewMin)
	SHADER_PARAMETER(FIntPoint, DepthViewSize)
	SHADER_PARAMETER(float, InputScale)
	SHADER_PARAMETER_ARRAY(FVector4f, LogLut, [256])
	SHADER_PARAMETER(float, LutMinK)
	SHADER_PARAMETER(float, LutMaxK)
	SHADER_PARAMETER(float, LutScale)
	SHADER_PARAMETER_ARRAY(FVector4f, ClassData, [32])
	SHADER_PARAMETER_ARRAY(FVector4f, StencilData, [128])
	SHADER_PARAMETER(uint32, NumClasses)
	SHADER_PARAMETER(uint32, TerrainClass)
	SHADER_PARAMETER(uint32, WaterClass)
	SHADER_PARAMETER(float, EntityDepthRatio)
	SHADER_PARAMETER(FMatrix44f, ClipToTranslatedWorld)
	SHADER_PARAMETER(FVector3f, Up)
	SHADER_PARAMETER(uint32, bWater)
	SHADER_PARAMETER(float, CamHeightCm)
	SHADER_PARAMETER(float, SeaRadiusCm)
	SHADER_PARAMETER(float, WaterBandCm)
	SHADER_PARAMETER(float, TairK)
	SHADER_PARAMETER(float, SkyEpsZ)
	SHADER_PARAMETER(float, Cloud)
	SHADER_PARAMETER(float, SkyHemiRadiance)
	SHADER_PARAMETER(float, BetaPerCm)
	SHADER_PARAMETER(float, KLum)
	SHADER_PARAMETER(float, EClampWm2)
	SHADER_PARAMETER(float, KFastScale)
	SHADER_PARAMETER(uint32, bBaseColorSrgb)
	SHADER_PARAMETER(uint32, UseBaseColor)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepth)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencil)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseColor)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutRadiance)
END_SHADER_PARAMETER_STRUCT()

/** Per-pixel temperature -> in-band radiance (ROADMAP 4A). */
class FCamSimThermalCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCamSimThermalCS);
	SHADER_USE_PARAMETER_STRUCT(FCamSimThermalCS, FGlobalShader);
	using FParameters = FCamSimThermalParameters;

	class FUseView : SHADER_PERMUTATION_BOOL("USE_VIEW");
	using FPermutationDomain = TShaderPermutationDomain<FUseView>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCamSimThermalCS, "/CamSim/Private/CamSimThermal.usf", "ThermalCS", SF_Compute);

bool IsThermalPassSupported(FString& OutWhy)
{
	if (GUsingNullRHI)
	{
		OutWhy = TEXT("NullRHI has no GPU");
		return false;
	}
	const FGlobalShaderMap* Map = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!Map)
	{
		OutWhy = TEXT("no global shader map");
		return false;
	}
	for (int32 Perm = 0; Perm < FCamSimThermalCS::FPermutationDomain::PermutationCount; ++Perm)
	{
		if (!Map->HasShader(&FCamSimThermalCS::GetStaticType(), Perm))
		{
			OutWhy = FString::Printf(TEXT("FCamSimThermalCS permutation %d missing from the global shader map"), Perm);
			return false;
		}
	}
	return true;
}

FRDGTextureRef AddThermalPass(FRDGBuilder& GraphBuilder, const FThermalPassInputs& In, const FThermalFrameParams& P)
{
	check(In.SceneColor && In.SceneDepth && In.CustomDepth && In.CustomStencil);
	check(In.SceneColorRect.Area() > 0);
	check(In.ViewUniformBuffer || In.DepthViewRect.Area() > 0);   // an empty rect leaves the shader's clamp undefined
	const FIntPoint Out = In.SceneColorRect.Size();
	const FRDGTextureRef Radiance = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(Out, PF_R32_FLOAT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("CamSimThermalRadiance"));

	const bool bUseView = In.ViewUniformBuffer != nullptr;
	auto* Pass = GraphBuilder.AllocParameters<FCamSimThermalParameters>();
	if (bUseView)
	{
		Pass->View = TUniformBufferRef<FViewUniformShaderParameters>(In.ViewUniformBuffer);
	}
	Pass->OutSize       = Out;
	Pass->ColorViewMin  = In.SceneColorRect.Min;
	Pass->DepthViewMin  = In.DepthViewRect.Min;
	Pass->DepthViewSize = In.DepthViewRect.Size();
	Pass->InputScale    = P.InputScale;
	for (int32 I = 0; I < FThermalFrameParams::LutSize; ++I) Pass->LogLut[I / 4][I % 4] = P.LogLut[I];
	Pass->LutMinK  = FThermalFrameParams::LutMinK;
	Pass->LutMaxK  = FThermalFrameParams::LutMaxK;
	Pass->LutScale = FThermalFrameParams::LutScale;
	for (int32 C = 0; C < FThermalFrameParams::MaxClasses; ++C)
	{
		Pass->ClassData[C] = FVector4f(P.ClassTempK[C], P.ClassEmissivity[C], P.ClassKFast[C], P.ClassSAbsRef[C]);
	}
	for (int32 K = 0; K < FThermalFrameParams::NumStencils / 2; ++K)
	{
		Pass->StencilData[K] = FVector4f(static_cast<float>(P.StencilClass[2 * K]), P.StencilOffsetK[2 * K],
			static_cast<float>(P.StencilClass[2 * K + 1]), P.StencilOffsetK[2 * K + 1]);
	}
	Pass->NumClasses       = P.NumClasses;
	Pass->TerrainClass     = P.TerrainClass;
	Pass->WaterClass       = P.WaterClass;
	Pass->EntityDepthRatio = P.EntityDepthRatio;
	Pass->ClipToTranslatedWorld = P.ClipToTranslatedWorld;
	Pass->Up               = P.Up;
	Pass->bWater           = P.bWater;
	Pass->CamHeightCm      = P.CamHeightCm;
	Pass->SeaRadiusCm      = P.SeaRadiusCm;
	Pass->WaterBandCm      = P.WaterBandCm;
	Pass->TairK            = P.TairK;
	Pass->SkyEpsZ          = P.SkyEpsZ;
	Pass->Cloud            = P.Cloud;
	Pass->SkyHemiRadiance  = P.SkyHemiRadiance;
	Pass->BetaPerCm        = P.BetaPerCm;
	Pass->KLum             = P.KLum;
	Pass->EClampWm2        = P.EClampWm2;
	Pass->KFastScale       = P.KFastScale;
	Pass->bBaseColorSrgb   = P.bBaseColorSrgb;
	Pass->UseBaseColor     = In.BaseColor ? 1u : 0u;
	Pass->SceneColor       = In.SceneColor;
	Pass->SceneDepth       = In.SceneDepth;
	Pass->CustomDepth      = In.CustomDepth;
	Pass->CustomStencil    = In.CustomStencil;
	Pass->BaseColor        = In.BaseColor ? In.BaseColor : GSystemTextures.GetBlackDummy(GraphBuilder);
	Pass->OutRadiance      = GraphBuilder.CreateUAV(Radiance);

	FCamSimThermalCS::FPermutationDomain Perm;
	Perm.Set<FCamSimThermalCS::FUseView>(bUseView);
	TShaderMapRef<FCamSimThermalCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Perm);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CamSimThermal %dx%d", Out.X, Out.Y), Shader, Pass,
		FComputeShaderUtils::GetGroupCount(Out, FIntPoint(8, 8)));
	return Radiance;
}
```

Then set the two constants in `ThermalPass.h` from the Task 1 record (`LINEAR` → `true`/`false`, `SRGB_ENCODED` → `true`/`true`, `FALLBACK` → `false`/`false`) and update the record line in `ROADMAP.md` only if it changed.

- [ ] **Step 5: Run the GPU test** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && scripts/run_gpu_tests.sh CamSim.GPU.Thermal` → Expected: `succeeded 1 failed 0` (first run compiles shaders for a few minutes). If a case fails, the message names the worst pixel's class: fix the HLSL to match the reference expression (never loosen the tolerance).
- [ ] **Step 6: Regression** — Run: `scripts/run_gpu_tests.sh CamSim.GPU && run_tests CamSim.Thermal` → Expected: every `CamSim.GPU.*` passes; `CamSim.Thermal` 0 failed.
- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Shaders/Private/CamSimThermalCommon.ush \
  unreal_project/CamSimTest/Shaders/Private/CamSimThermal.usf \
  unreal_project/CamSimTest/Source/CamSimShaders/Public/ThermalPass.h \
  unreal_project/CamSimTest/Source/CamSimShaders/Private/ThermalPass.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalGpuTest.cpp
git commit -F - <<'EOF'
feat(thermal): ThermalCS — per-pixel class, temperature and in-band radiance, matched to the CPU reference

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 9: Per-frame parameters (`FThermalFrameBuilder`)

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameBuilder.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameBuilder.cpp`
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalBuilderTest.cpp`

**Interfaces:**
- Consumes: `FBandRadiance` (Task 2), `FThermalMaterialTable` (Task 3), `FCamSimConfig::FThermalConfig` (Task 3), `FThermalSky` (Task 5), `FThermalModel`, `FThermalSite` (Task 6), `FThermalFrameParams` (Task 7), `FSimClock::FromMicros(uint64)` (existing, `Time/SimClock.h`), `CamSimThermalRef::EvaluatePixel` + `CamSimThermalTest` helpers (Task 7, tests only).
- Produces:
  ```cpp
  struct FThermalStencilEntity { uint8 Stencil = 0; FString ThermalMaterial; TOptional<float> ThermalOffsetK; bool bSurfaceVehicle = false; };
  struct FThermalFrameInputs
  {
      uint64 UtcMicros = 0; double CamLatDeg = 0.0, CamLonDeg = 0.0, CamAltHaeM = 0.0;
      double AirTempC = 15.0, CloudCover01 = 0.0, VisibilityM = 10000.0; bool bFogActive = false;
      double WaterTempC = 15.0; bool bHasSea = false; double SeaLevelHaeM = 0.0, MaxWaveAmplitudeM = 0.0;
      FVector UpWorld = FVector::UpVector; double SunIlluminanceLux = 0.0;
      bool bBaseColorAvailable = true, bBaseColorSrgb = false;
      TArray<FThermalStencilEntity> Entities;
  };
  class CAMSIMTEST_API FThermalFrameBuilder
  {
  public:
      static constexpr float  DefaultVehicleOffsetK = 8.0f;
      static constexpr double FogVisibilityK = 3.912, WaterBandBaseM = 0.5, MwirMaxCentreUm = 6.5;
      void  Configure(const FCamSimConfig::FThermalConfig& Cfg, float BandLoUm, float BandHiUm);
      void  Build(const FThermalFrameInputs& In, FThermalFrameParams& Out, TArray<FString>* OutWarnings = nullptr);
      float GetSignalScale() const;           // 1 / B(300 K)
      bool  IsMwir() const;
      const FBandRadiance& GetBand() const;
      const FThermalMaterialTable& GetMaterials() const;
      static double LocalSolarSeconds(uint64 UtcMicros, double LonDeg);   // [0, 86400)
      static FDateTime LocalSolarDate(uint64 UtcMicros, double LonDeg);
      static double GaussianRadiusM(double LatDeg);                       // WGS-84 sqrt(M N)
  };
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalBuilderTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalMaterials.h"
#include "Thermal/ThermalReference.h"
#include "Time/SimClock.h"
#include "Tests/ThermalTestScene.h"

// CamSim.Thermal.Builder.*: per-frame FThermalFrameParams from clock, environment, camera and entities (ROADMAP 4A).

namespace
{
	/** San Francisco (Presidio), 21 Dec 2026, UTC hh:mm: 20:10 is local solar noon, 10:10 is 02:00. */
	FThermalFrameInputs SanFrancisco(int32 UtcHour, int32 UtcMinute)
	{
		FThermalFrameInputs In;
		In.UtcMicros = FSimClock::ToMicros(FDateTime(2026, 12, 21, UtcHour, UtcMinute));
		In.CamLatDeg = 37.7989; In.CamLonDeg = -122.4662; In.CamAltHaeM = 342.0;
		In.AirTempC = 15.0; In.WaterTempC = 15.0;
		In.bHasSea = true; In.SeaLevelHaeM = -32.0; In.MaxWaveAmplitudeM = 0.4;
		In.SunIlluminanceLux = 100000.0;
		return In;
	}

	FThermalFrameBuilder MakeBuilder(float Lo = 3.0f, float Hi = 5.0f)
	{
		FThermalFrameBuilder B;
		B.Configure(FCamSimConfig::FThermalConfig(), Lo, Hi);
		return B;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderSignsTest, "CamSim.Thermal.Builder.NightAndNoonSigns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderSignsTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameParams Noon, Night;
	B.Build(SanFrancisco(20, 10), Noon);
	B.Build(SanFrancisco(10, 10), Night);
	const int32 T = FThermalMaterialTable::TerrainDefault, W = FThermalMaterialTable::Water;
	TestTrue(*FString::Printf(TEXT("noon: terrain %.1f K > water %.1f K"), Noon.ClassTempK[T], Noon.ClassTempK[W]), Noon.ClassTempK[T] > Noon.ClassTempK[W]);
	TestTrue(*FString::Printf(TEXT("night: terrain %.1f K < water %.1f K"), Night.ClassTempK[T], Night.ClassTempK[W]), Night.ClassTempK[T] < Night.ClassTempK[W]);
	TestTrue(TEXT("noon reference flux > 0"), Noon.ClassSAbsRef[T] > 100.0f && Noon.EClampWm2 > 100.0f);
	TestEqual(TEXT("night reference flux 0"), Night.ClassSAbsRef[T], 0.0f);
	TestEqual(TEXT("night E clamp 0"), Night.EClampWm2, 0.0f);
	TestEqual(TEXT("classes"), static_cast<int32>(Noon.NumClasses), B.GetMaterials().Num());
	TestEqual(TEXT("terrain class"), Noon.TerrainClass, static_cast<uint32>(FThermalMaterialTable::TerrainDefault));
	TestEqual(TEXT("water class"), Noon.WaterClass, static_cast<uint32>(FThermalMaterialTable::Water));
	TestTrue(TEXT("sky colder than air"), Noon.SkyHemiRadiance < B.GetBand().Radiance(Noon.TairK));
	TestNearlyEqual(TEXT("signal scale = 1 / B(300 K)"), B.GetSignalScale(), 1.0f / B.GetBand().Radiance(300.0f), 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderStencilTest, "CamSim.Thermal.Builder.StencilTableReuseAndUnknownMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderStencilTest::RunTest(const FString& Parameters)
{
	// Review focus 4.
	FThermalFrameBuilder B = MakeBuilder();
	const FThermalMaterialTable& M = B.GetMaterials();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	FThermalStencilEntity Truck; Truck.Stencil = 3; Truck.bSurfaceVehicle = true;
	FThermalStencilEntity Jet;   Jet.Stencil = 4;
	FThermalStencilEntity Paved; Paved.Stencil = 5; Paved.ThermalMaterial = TEXT("asphalt"); Paved.ThermalOffsetK = -2.0f;
	FThermalStencilEntity Typo;  Typo.Stencil = 6; Typo.ThermalMaterial = TEXT("unobtainium");
	In.Entities = { Truck, Jet, Paved, Typo };
	FThermalFrameParams P;
	TArray<FString> Warnings;
	B.Build(In, P, &Warnings);
	TestEqual(TEXT("truck: vehicle_paint"), static_cast<int32>(P.StencilClass[3]), FThermalMaterialTable::VehiclePaint);
	TestEqual(TEXT("truck: +8 K (surface vehicle)"), P.StencilOffsetK[3], 8.0f);
	TestEqual(TEXT("jet: 0 K"), P.StencilOffsetK[4], 0.0f);
	TestEqual(TEXT("asphalt class"), static_cast<int32>(P.StencilClass[5]), M.Find(TEXT("asphalt")));
	TestEqual(TEXT("explicit offset"), P.StencilOffsetK[5], -2.0f);
	TestEqual(TEXT("unknown material: vehicle_paint"), static_cast<int32>(P.StencilClass[6]), FThermalMaterialTable::VehiclePaint);
	TestEqual(TEXT("one warning"), Warnings.Num(), 1);
	TestTrue(TEXT("warning names it"), Warnings.Num() == 1 && Warnings[0].Contains(TEXT("unobtainium")));
	int32 OutOfRange = 0;
	for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S) OutOfRange += P.StencilClass[S] < P.NumClasses ? 0 : 1;
	TestEqual(TEXT("no class index >= NumClasses"), OutOfRange, 0);

	// Stencil 3 released and reused 4 frames later by a different type; stencil 5 gone.
	FThermalStencilEntity Boat; Boat.Stencil = 3; Boat.ThermalMaterial = TEXT("concrete");
	In.Entities = { Boat, Typo };
	Warnings.Reset();
	B.Build(In, P, &Warnings);
	TestEqual(TEXT("reused stencil: the new entity's class"), static_cast<int32>(P.StencilClass[3]), M.Find(TEXT("concrete")));
	TestEqual(TEXT("reused stencil: the new entity's offset"), P.StencilOffsetK[3], 0.0f);
	TestEqual(TEXT("released stencil back to the default"), static_cast<int32>(P.StencilClass[5]), FThermalMaterialTable::VehiclePaint);
	TestEqual(TEXT("released stencil offset 0"), P.StencilOffsetK[5], 0.0f);
	TestEqual(TEXT("the unknown name warns once per session"), Warnings.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderSeaTest, "CamSim.Thermal.Builder.SeaGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderSeaTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	In.UpWorld = FVector(0.0, 0.1, 1.0);
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("water on"), P.bWater, 1u);
	TestNearlyEqual(TEXT("camera height above the sea"), P.CamHeightCm, (342.0f + 32.0f) * 100.0f, 0.5f);
	TestNearlyEqual(TEXT("band = 0.5 m + max amplitude"), P.WaterBandCm, 90.0f, 1e-3f);
	TestNearlyEqual(TEXT("Gaussian radius at 37.8 deg"), P.SeaRadiusCm, static_cast<float>(FThermalFrameBuilder::GaussianRadiusM(37.7989) * 100.0), 1.0f);
	TestTrue(TEXT("radius ~ 6.37e8 cm"), P.SeaRadiusCm > 6.35e8f && P.SeaRadiusCm < 6.39e8f);
	TestNearlyEqual(TEXT("up normalised"), P.Up.Size(), 1.0f, 1e-6f);
	In.bHasSea = false;
	B.Build(In, P);
	TestEqual(TEXT("no sea: no water class"), P.bWater, 0u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderKLumTest, "CamSim.Thermal.Builder.KLumReferenceSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderKLumTest::RunTest(const FString& Parameters)
{
	// An unshadowed horizontal surface of the class albedo lit by the sun light (lux * sin el) gets S_abs,pix = S_abs,ref:
	// no fast term, through the real reference.
	FThermalFrameBuilder B = MakeBuilder();
	for (const double Cloud : { 0.0, 0.6 })
	{
		FThermalFrameInputs In = SanFrancisco(20, 10);
		In.bHasSea = false;
		In.CloudCover01 = Cloud;
		FThermalFrameParams P;
		B.Build(In, P);
		const FRotator Rot(-30.0, 0.0, 0.0);
		P.ClipToTranslatedWorld = CamSimThermalRef::MakeClipToTranslatedWorld(Rot, 60.0f, 64, 36, CamSimThermalTest::NearCm);
		const float A = B.GetMaterials().Get(FThermalMaterialTable::TerrainDefault).Albedo;
		const double SinEl = FMath::Sin(FMath::DegreesToRadians(FThermalModel::SunElevationDeg(B.GetModel().GetSite(),
			FThermalFrameBuilder::LocalSolarSeconds(In.UtcMicros, In.CamLonDeg) / 3600.0)));
		CamSimThermalRef::FPixelSample S;
		S.U = 0.5f; S.V = 0.6f;
		S.DeviceZ = CamSimThermalTest::DeviceZForPlane(P, Rot, S.U, S.V, -700.0f);
		S.bHasBase = true;
		S.Base = FVector3f(A);
		S.Color = FVector3f(static_cast<float>(A * In.SunIlluminanceLux * SinEl / UE_DOUBLE_PI));
		const CamSimThermalRef::FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
		TestNearlyEqual(*FString::Printf(TEXT("cloud %.1f: reference surface keeps the class temperature"), Cloud),
			R.TempK, P.ClassTempK[FThermalMaterialTable::TerrainDefault], 1e-2f);
	}
	FThermalFrameInputs Night = SanFrancisco(10, 10);
	FThermalFrameParams P;
	B.Build(Night, P);
	TestTrue(TEXT("night K_lum finite and > 0"), FMath::IsFinite(P.KLum) && P.KLum > 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderExtinctionTest, "CamSim.Thermal.Builder.Extinction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderExtinctionTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder Mwir = MakeBuilder(3.0f, 5.0f), Lwir = MakeBuilder(8.0f, 12.0f);
	TestTrue(TEXT("3-5 um is MWIR"), Mwir.IsMwir());
	TestFalse(TEXT("8-12 um is LWIR"), Lwir.IsMwir());
	FThermalFrameInputs In = SanFrancisco(20, 10);
	FThermalFrameParams P;
	Mwir.Build(In, P);
	TestNearlyEqual(TEXT("MWIR beta"), P.BetaPerCm, 0.15f / 1e5f, 1e-9f);
	Lwir.Build(In, P);
	TestNearlyEqual(TEXT("LWIR beta"), P.BetaPerCm, 0.10f / 1e5f, 1e-9f);
	In.bFogActive = true;
	In.VisibilityM = 2000.0;
	Mwir.Build(In, P);
	TestNearlyEqual(TEXT("fog: + 3.912 / V_km * 0.4"), P.BetaPerCm, static_cast<float>((0.15 + 3.912 / 2.0 * 0.4) / 1e5), 1e-9f);
	In.VisibilityM = 0.0;
	Mwir.Build(In, P);
	TestTrue(TEXT("zero visibility stays finite"), FMath::IsFinite(P.BetaPerCm));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderFallbackTest, "CamSim.Thermal.Builder.FallbackFastTerm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderFallbackTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	FThermalFrameParams P;
	B.Build(In, P);
	TestEqual(TEXT("base colour + sun light: fast term on"), P.KFastScale, 1.0f);
	In.bBaseColorAvailable = false;
	B.Build(In, P);
	TestEqual(TEXT("no base colour (Task 1 FALLBACK): k_fast = 0"), P.KFastScale, 0.0f);
	In.bBaseColorAvailable = true;
	In.SunIlluminanceLux = 0.0;
	B.Build(In, P);
	TestEqual(TEXT("no atmosphere sun light: k_fast = 0"), P.KFastScale, 0.0f);
	In.SunIlluminanceLux = 1e5;
	In.bBaseColorSrgb = true;
	B.Build(In, P);
	TestEqual(TEXT("sRGB flag passed through"), P.bBaseColorSrgb, 1u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderDateLineTest, "CamSim.Thermal.Builder.DateLineLocalTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderDateLineTest::RunTest(const FString& Parameters)
{
	// Review focus 3: local solar time and date across the date line, near UTC midnight.
	const uint64 Late = FSimClock::ToMicros(FDateTime(2026, 12, 31, 23, 59, 59));
	const double East = FThermalFrameBuilder::LocalSolarSeconds(Late, 179.9);
	const double West = FThermalFrameBuilder::LocalSolarSeconds(Late, -179.9);
	TestTrue(TEXT("east of Greenwich: in [0, 86400)"), East >= 0.0 && East < 86400.0);
	TestTrue(TEXT("west of Greenwich: in [0, 86400)"), West >= 0.0 && West < 86400.0);
	TestNearlyEqual(TEXT("+179.9: 11:59:35 next day"), East, FMath::Fmod(86399.0 + 179.9 * 240.0, 86400.0), 1e-3);
	TestEqual(TEXT("+179.9 rolls into 2027"), FThermalFrameBuilder::LocalSolarDate(Late, 179.9).GetYear(), 2027);
	TestEqual(TEXT("-179.9 stays 31 Dec"), FThermalFrameBuilder::LocalSolarDate(Late, -179.9).GetDayOfYear(), 365);
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(23, 59);
	In.UtcMicros = Late;
	for (const double Lon : { 179.9, -179.9 })
	{
		In.CamLonDeg = Lon;
		FThermalFrameParams P;
		B.Build(In, P);
		bool bFinite = FMath::IsFinite(P.KLum) && FMath::IsFinite(P.SkyHemiRadiance);
		for (uint32 C = 0; C < P.NumClasses; ++C) bFinite &= FMath::IsFinite(P.ClassTempK[C]) && P.ClassTempK[C] > 150.0f && P.ClassTempK[C] < 400.0f;
		TestTrue(*FString::Printf(TEXT("lon %.1f: finite, in range"), Lon), bFinite);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalBuilderCostTest, "CamSim.Thermal.Builder.PerFrameCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalBuilderCostTest::RunTest(const FString& Parameters)
{
	FThermalFrameBuilder B = MakeBuilder();
	FThermalFrameInputs In = SanFrancisco(20, 10);
	for (int32 S = 1; S <= 32; ++S) { FThermalStencilEntity E; E.Stencil = static_cast<uint8>(S); E.bSurfaceVehicle = true; In.Entities.Add(E); }
	FThermalFrameParams P;
	B.Build(In, P);   // fits the model and builds the LUT once
	constexpr int32 Frames = 1000;
	const double T0 = FPlatformTime::Seconds();
	for (int32 F = 0; F < Frames; ++F)
	{
		In.UtcMicros += 33333;
		B.Build(In, P);
	}
	const double UsPerFrame = (FPlatformTime::Seconds() - T0) * 1e6 / Frames;
	AddInfo(FString::Printf(TEXT("builder: %.1f us per frame (budget 50 us)"), UsPerFrame));
	if (UsPerFrame > 50.0) AddWarning(FString::Printf(TEXT("builder over the 50 us budget: %.1f us"), UsPerFrame));
	TestTrue(TEXT("builder per frame well under a millisecond"), UsPerFrame < 500.0);
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `Thermal/ThermalFrameBuilder.h` not found.

- [ ] **Step 3: Implement** — `Source/CamSimTest/Thermal/ThermalFrameBuilder.h`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"
#include "ThermalFrameParams.h"
#include "Thermal/BandRadiance.h"
#include "Thermal/ThermalMaterials.h"
#include "Thermal/ThermalModel.h"

/** One stencil-tagged entity, as the entity manager reports it (ROADMAP 4A). */
struct FThermalStencilEntity
{
	uint8   Stencil = 0;              // custom-depth stencil 1..255
	FString ThermalMaterial;          // entity_types.<id>.thermal_material; empty = vehicle_paint
	TOptional<float> ThermalOffsetK;  // entity_types.<id>.thermal_offset_k; unset = +8 K for a surface vehicle, else 0
	bool    bSurfaceVehicle = false;  // land/sea vehicle (placed on the surface)
};

/** The sim state one frame of thermal parameters is built from (CamSimThermal::GatherFrameInputs fills it). */
struct FThermalFrameInputs
{
	uint64  UtcMicros  = 0;              // FSimClock::NowMicros
	double  CamLatDeg  = 0.0;
	double  CamLonDeg  = 0.0;
	double  CamAltHaeM = 0.0;
	double  AirTempC     = 15.0;         // daily mean (CIGI Atmosphere Control / thermal.air_temperature_c)
	double  CloudCover01 = 0.0;
	double  VisibilityM  = 10000.0;
	bool    bFogActive   = false;        // CIGI Atmosphere Control fog enabled
	double  WaterTempC   = 15.0;
	bool    bHasSea      = false;        // FOceanSurface present with a sea level here
	double  SeaLevelHaeM = 0.0;          // still sea level (EGM96 + tide), WGS-84 m
	double  MaxWaveAmplitudeM = 0.0;     // sum of the active waves' amplitudes
	FVector UpWorld = FVector::UpVector; // geodetic up at the camera, UE world (= translated world direction)
	double  SunIlluminanceLux = 0.0;     // atmosphere sun light at the ground, on a surface facing it; 0 = none found
	bool    bBaseColorAvailable = true;  // CamSimThermalPass::bBaseColorAtTonemapper
	bool    bBaseColorSrgb = false;      // CamSimThermalPass::bBaseColorSrgbEncoded
	TArray<FThermalStencilEntity> Entities;
};

/**
 * Fills FThermalFrameParams each frame (ROADMAP 4A): LUT for the preset's band, class temperatures from FThermalModel
 * at the local solar time, sky and path terms, K_lum, sea geometry, and the 256-entry stencil table rebuilt from the
 * live entities. ClipToTranslatedWorld is left for the render thread. Game thread.
 */
class CAMSIMTEST_API FThermalFrameBuilder
{
public:
	static constexpr float  DefaultVehicleOffsetK = 8.0f;
	static constexpr double FogVisibilityK  = 3.912;   // Koschmieder: beta = 3.912 / V
	static constexpr double WaterBandBaseM  = 0.5;
	static constexpr double MwirMaxCentreUm = 6.5;     // band centre below this: MWIR extinction

	/** Rebuild the LUT when the band changes and the material table when thermal.materials changes. */
	void Configure(const FCamSimConfig::FThermalConfig& Cfg, float BandLoUm, float BandHiUm);
	/** This frame's parameters. New warnings (each at most once per builder) are appended to OutWarnings. */
	void Build(const FThermalFrameInputs& In, FThermalFrameParams& Out, TArray<FString>* OutWarnings = nullptr);

	/** Detector signal scale: L / B(300 K) (~1 for a 300 K scene). */
	float GetSignalScale() const { return 1.0f / Band.Radiance(300.0f); }
	bool  IsMwir() const { return 0.5 * (Band.GetLoUm() + Band.GetHiUm()) < MwirMaxCentreUm; }
	const FBandRadiance& GetBand() const { return Band; }
	const FThermalMaterialTable& GetMaterials() const { return Materials; }
	const FThermalModel& GetModel() const { return Model; }

	static double LocalSolarSeconds(uint64 UtcMicros, double LonDeg);
	static FDateTime LocalSolarDate(uint64 UtcMicros, double LonDeg);
	static double GaussianRadiusM(double LatDeg);

private:
	FCamSimConfig::FThermalConfig Config;
	bool                  bConfigured = false;
	FBandRadiance         Band;
	FThermalMaterialTable Materials;
	FThermalModel         Model;
	TArray<FString>       PendingWarnings;   // material table errors, reported by the next Build
	TSet<FString>         WarnedMaterials;
};
```

`Source/CamSimTest/Thermal/ThermalFrameBuilder.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalSky.h"
#include "Time/SimClock.h"

static_assert(FThermalMaterialTable::MaxClasses == FThermalFrameParams::MaxClasses, "class table sizes must agree");

double FThermalFrameBuilder::LocalSolarSeconds(uint64 UtcMicros, double LonDeg)
{
	const double S = FMath::Fmod(static_cast<double>(UtcMicros) * 1e-6 + LonDeg * 240.0, FThermalModel::DaySeconds);
	return S < 0.0 ? S + FThermalModel::DaySeconds : S;
}

FDateTime FThermalFrameBuilder::LocalSolarDate(uint64 UtcMicros, double LonDeg)
{
	return FSimClock::FromMicros(UtcMicros) + FTimespan::FromSeconds(LonDeg * 240.0);
}

double FThermalFrameBuilder::GaussianRadiusM(double LatDeg)
{
	constexpr double A = 6378137.0, E2 = 6.69437999014e-3;
	const double S = FMath::Sin(FMath::DegreesToRadians(LatDeg));
	const double W = 1.0 - E2 * S * S;
	const double M = A * (1.0 - E2) / (W * FMath::Sqrt(W));   // meridional
	const double N = A / FMath::Sqrt(W);                       // prime vertical
	return FMath::Sqrt(M * N);
}

void FThermalFrameBuilder::Configure(const FCamSimConfig::FThermalConfig& Cfg, float BandLoUm, float BandHiUm)
{
	if (!Band.IsBuilt() || Band.GetLoUm() != BandLoUm || Band.GetHiUm() != BandHiUm)
	{
		Band.Build(BandLoUm, BandHiUm);
	}
	if (!bConfigured || !(Config.Materials == Cfg.Materials))
	{
		PendingWarnings.Append(Materials.Build(Cfg.Materials));
	}
	Config = Cfg;
	bConfigured = true;
}

void FThermalFrameBuilder::Build(const FThermalFrameInputs& In, FThermalFrameParams& Out, TArray<FString>* OutWarnings)
{
	check(bConfigured);
	auto Warn = [OutWarnings](FString W) { if (OutWarnings) OutWarnings->Add(MoveTemp(W)); };
	for (FString& W : PendingWarnings) Warn(MoveTemp(W));
	PendingWarnings.Reset();

	// Site and time (local solar).
	const double LocalSec = LocalSolarSeconds(In.UtcMicros, In.CamLonDeg);
	const double Hour = LocalSec / 3600.0;
	const FDateTime Date = LocalSolarDate(In.UtcMicros, In.CamLonDeg);
	FThermalSite Site;
	Site.Year      = Date.GetYear();
	Site.DayOfYear = Date.GetDayOfYear();
	Site.LatDeg    = In.CamLatDeg;
	Site.LonDeg    = In.CamLonDeg;
	Site.TairMeanK = In.AirTempC + 273.15;
	Site.AirSwingK = Config.AirDiurnalSwingK;
	Site.Cloud     = FMath::Clamp(In.CloudCover01, 0.0, 1.0);
	Model.Update(Site, Materials);

	const double TairK  = FThermalModel::AirTemperatureK(Site, Hour);
	const double SunEl  = FThermalModel::SunElevationDeg(Site, Hour);
	const double SClear = FThermalModel::ClearSkyGhi(SunEl);
	const double SRef0  = SClear * FThermalModel::CloudFactor(Site.Cloud);   // what a horizontal class surface absorbs / (1 - a)
	const double WaterK = In.WaterTempC + 273.15;

	// LUT and classes.
	FMemory::Memcpy(Out.LogLut, Band.GetLogLut(), sizeof(Out.LogLut));
	Out.NumClasses = static_cast<uint32>(Materials.Num());
	for (int32 C = 0; C < Materials.Num(); ++C)
	{
		const FThermalMaterial& M = Materials.Get(C);
		Out.ClassTempK[C]      = static_cast<float>(Model.TemperatureK(C, LocalSec, WaterK));
		Out.ClassEmissivity[C] = M.Emissivity;
		Out.ClassKFast[C]      = M.KFast;
		Out.ClassSAbsRef[C]    = static_cast<float>((1.0 - M.Albedo) * SRef0);
	}
	Out.TerrainClass = FThermalMaterialTable::TerrainDefault;
	Out.WaterClass   = FThermalMaterialTable::Water;

	// Stencil table: rebuilt from the live entities every frame (a released stencil reverts to the default).
	for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S)
	{
		Out.StencilClass[S]   = static_cast<uint8>(FThermalMaterialTable::VehiclePaint);
		Out.StencilOffsetK[S] = 0.0f;
	}
	for (const FThermalStencilEntity& E : In.Entities)
	{
		if (E.Stencil == 0) continue;
		int32 Class = FThermalMaterialTable::VehiclePaint;
		if (!E.ThermalMaterial.IsEmpty())
		{
			Class = Materials.Find(E.ThermalMaterial);
			if (Class == INDEX_NONE)
			{
				Class = FThermalMaterialTable::VehiclePaint;
				if (!WarnedMaterials.Contains(E.ThermalMaterial))
				{
					WarnedMaterials.Add(E.ThermalMaterial);
					Warn(FString::Printf(TEXT("entity_types thermal_material '%s' is not a thermal class; using vehicle_paint (warned once)"), *E.ThermalMaterial));
				}
			}
		}
		Out.StencilClass[E.Stencil]   = static_cast<uint8>(Class);
		Out.StencilOffsetK[E.Stencil] = E.ThermalOffsetK.IsSet() ? *E.ThermalOffsetK : (E.bSurfaceVehicle ? DefaultVehicleOffsetK : 0.0f);
	}
	Out.EntityDepthRatio = 0.99f;

	// Sky and atmosphere.
	Out.TairK           = static_cast<float>(TairK);
	Out.SkyEpsZ         = static_cast<float>(FThermalSky::ZenithEmissivity(TairK));
	Out.Cloud           = static_cast<float>(Site.Cloud);
	Out.SkyHemiRadiance = static_cast<float>(FThermalSky::HemisphereBandRadiance(Band, TairK, Site.Cloud));
	double BetaKm = IsMwir() ? Config.ExtinctionPerKmMwir : Config.ExtinctionPerKmLwir;
	if (In.bFogActive)
	{
		BetaKm += FogVisibilityK / FMath::Max(In.VisibilityM / 1000.0, 0.01) * Config.FogIrFactor;
	}
	Out.BetaPerCm = static_cast<float>(BetaKm / 1e5);

	// Solar fast term: K_lum maps the sun light's horizontal illuminance onto the class model's reference flux.
	const double SinEl = FMath::Max(FMath::Sin(FMath::DegreesToRadians(SunEl)), 0.0);
	const double SunLuxHoriz = FMath::Max(In.SunIlluminanceLux, 0.0) * SinEl;
	Out.KLum       = static_cast<float>(SRef0 > 1.0 ? FMath::Max(SunLuxHoriz, 1e-3) / SRef0 : 1.0);
	Out.EClampWm2  = static_cast<float>(1.5 * SClear);
	Out.KFastScale = (In.bBaseColorAvailable && In.SunIlluminanceLux > 0.0) ? 1.0f : 0.0f;
	Out.bBaseColorSrgb = In.bBaseColorSrgb ? 1u : 0u;

	// Geometry (the render thread sets ClipToTranslatedWorld).
	Out.Up          = FVector3f(In.UpWorld.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector));
	Out.bWater      = In.bHasSea ? 1u : 0u;
	Out.CamHeightCm = In.bHasSea ? static_cast<float>((In.CamAltHaeM - In.SeaLevelHaeM) * 100.0) : 0.0f;
	Out.SeaRadiusCm = static_cast<float>(GaussianRadiusM(In.CamLatDeg) * 100.0);
	Out.WaterBandCm = static_cast<float>((WaterBandBaseM + FMath::Max(In.MaxWaveAmplitudeM, 0.0)) * 100.0);
	Out.InputScale  = 1.0f;
}
```

- [ ] **Step 4: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Thermal.Builder` → Expected: `succeeded 8 failed 0` (`PerFrameCost` logs its µs; a value over 50 µs is a warning to look at, not a failure).

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameBuilder.h \
  unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameBuilder.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalBuilderTest.cpp
git commit -F - <<'EOF'
feat(thermal): FThermalFrameBuilder — class temperatures, sky, path, K_lum, sea and stencil table per frame

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 10: Sensor controller — thermal AE slot, interpolated percentiles, capped AGC with margin

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorController.h`, `unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorController.cpp`
- Test: modify `unreal_project/CamSimTest/Source/CamSimTest/Tests/SensorControllerTest.cpp` (append)

**Interfaces:**
- Consumes: `FSensorModeConfig::{ThermalExposure, AGCMaxDisplayGain}` (Task 4).
- Produces:
  ```cpp
  // FSensorControllerInput
  bool bRadianceInput = false;    // IR thermal radiance (ROADMAP 4A)
  // FSensorController
  static constexpr float RadianceSeedGainEv = -1.0f;
  static constexpr float ThermalAgcMargin = 0.1f;
  static float TotalGainCapEv(const FSensorModeConfig& Cfg, const FSensorExposureConfig& E);
  static bool  PercentileLog2Interp(const FSensorHistogram& H, float P, float& OutLog2);
  ```
  Behaviour: with `Mode == IR && bRadianceInput` the AE uses `Cfg.ThermalExposure` and its own state slot (seed `RadianceSeedGainEv`); switching between the luminance and radiance inputs snaps like a mode switch; the AGC uses interpolated percentiles, maps its band to `[ThermalAgcMargin, 1 - ThermalAgcMargin]`, and caps `DisplayGain` at `Cfg.AGCMaxDisplayGain` (band centred on 0.5 when capped). Without `bRadianceInput` nothing changes.

- [ ] **Step 1: Write the failing tests** — append to `Tests/SensorControllerTest.cpp`:

```cpp
// ---------------------------------------------------------------------------
// ROADMAP 4A: thermal radiance input (signal = L / B(300 K), ~1 for a 300 K scene)
// ---------------------------------------------------------------------------

namespace
{
	FSensorModeConfig ThermalCfg()
	{
		FSensorModeConfig C;
		C.bAGCEnabled = true;
		C.AGCLowPercentile = 0.01f;
		C.AGCHighPercentile = 0.99f;
		C.AGCLagFrames = 0;
		C.ThermalExposure.LagFrames = 0;
		C.Detector.MaxAnalogGainDb = 0.0f;
		return C;
	}

	FSensorControllerInput Radiance(const FSensorHistogram* H, uint32 Serial)
	{
		FSensorControllerInput I = In(H, Serial);
		I.Mode = ESensorGraphMode::IR;
		I.bRadianceInput = true;
		return I;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorThermalExposureTest, "CamSim.Sensor.Controller.RadianceUsesThermalExposure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorThermalExposureTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = ThermalCfg();
	Cfg.bAGCEnabled = false;
	const FSensorHistogram H = Flat(0.0f, 1);   // a 300 K scene
	const float SceneLog2 = FSensorHistogram::BinCentreLog2(FSensorHistogram::BinOf(1.0f));
	const FSensorFrameParams P = C.Update(Radiance(&H, 1), Cfg);
	TestEqual(TEXT("300 K scene exposed to mid-range (thermal target grey 0.5)"), FMath::Exp2(SceneLog2) * P.PhotonGain, 0.5f, 0.5f * 0.01f);
	TestTrue(TEXT("inside the thermal limits [-8, 0] EV"), C.GetGainEv() >= -8.0f && C.GetGainEv() <= 0.0f);
	const FSensorHistogram Cold = Flat(-6.0f, 2);   // far too dark for the thermal camera: clamps at its max gain
	FSensorControllerInput I = Radiance(&Cold, 2);
	I.bCameraCut = true;
	C.Update(I, Cfg);
	TestEqual(TEXT("clamped at thermal max_photon_gain_ev (0)"), C.GetGainEv(), 0.0f, 1e-4f);
	TestEqual(TEXT("TotalGainCapEv with the thermal block"), FSensorController::TotalGainCapEv(Cfg, Cfg.ThermalExposure), 0.0f);
	TestEqual(TEXT("TotalGainCapEv (luminance) unchanged"), FSensorController::TotalGainCapEv(Cfg), Cfg.Exposure.MaxPhotonGainEv);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRadianceSlotTest, "CamSim.Sensor.Controller.RadianceSlotIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRadianceSlotTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = ThermalCfg();
	Cfg.bAGCEnabled = false;
	Cfg.Exposure.LagFrames = 0;
	FSensorControllerInput Lum = In(nullptr, 1);
	Lum.Mode = ESensorGraphMode::IR;
	const FSensorHistogram Night = Flat(4.0f, 1);
	Lum.NewHistogram = &Night;
	const float LumPhoton = C.Update(Lum, Cfg).PhotonGain;
	const FSensorHistogram Warm = Flat(0.0f, 2);
	C.Update(Radiance(&Warm, 2), Cfg);
	C.Update(Radiance(&Warm, 3), Cfg);
	TestTrue(TEXT("radiance slot converged elsewhere"), FMath::Abs(FMath::Log2(LumPhoton) - C.GetGainEv()) > 1.0f);
	FSensorControllerInput Back = In(nullptr, 4);   // no new histogram: the luminance slot's state is emitted
	Back.Mode = ESensorGraphMode::IR;
	TestEqual(TEXT("luminance slot untouched by radiance frames"), C.Update(Back, Cfg).PhotonGain, LumPhoton, LumPhoton * 1e-5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorThermalAgcInterpTest, "CamSim.Sensor.Controller.ThermalAgcInterpolatedPercentiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorThermalAgcInterpTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	const FSensorModeConfig Cfg = ThermalCfg();
	FSensorHistogram H;
	const int32 B = FSensorHistogram::BinOf(1.0f);
	H.Bins[B] = 600; H.Bins[B + 1] = 400; H.Serial = 1;
	const FSensorFrameParams P = C.Update(Radiance(&H, 1), Cfg);
	// 1 %: 10 of bin B's 600; 99 %: 390 of bin B+1's 400 (uniform within a bin).
	const float Lo = FSensorHistogram::MinLog2 + (B + 10.0f / 600.0f) / FSensorHistogram::BinsPerStop;
	const float Hi = FSensorHistogram::MinLog2 + (B + 1 + 390.0f / 400.0f) / FSensorHistogram::BinsPerStop;
	const float N = P.PhotonGain * P.AnalogGain;
	TestEqual(TEXT("low percentile -> margin (0.1)"), FMath::Exp2(Lo) * N * P.DisplayGain + P.DisplayOffset, FSensorController::ThermalAgcMargin, 1e-4f);
	TestEqual(TEXT("high percentile -> 1 - margin (0.9)"), FMath::Exp2(Hi) * N * P.DisplayGain + P.DisplayOffset, 1.0f - FSensorController::ThermalAgcMargin, 1e-4f);
	float Out = 0.0f;
	TestTrue(TEXT("interp percentile"), FSensorController::PercentileLog2Interp(H, 0.3f, Out));
	TestEqual(TEXT("30 % = half of bin B"), Out, FSensorHistogram::MinLog2 + (B + 0.5f) / FSensorHistogram::BinsPerStop, 1e-5f);
	TestFalse(TEXT("empty histogram"), FSensorController::PercentileLog2Interp(FSensorHistogram(), 0.5f, Out));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorThermalAgcCapTest, "CamSim.Sensor.Controller.ThermalAgcFlatSceneCapped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorThermalAgcCapTest::RunTest(const FString& Parameters)
{
	// Review focus 1: a flat night scene (every pixel in one bin) and a bimodal land/water frame.
	FSensorModeConfig Cfg = ThermalCfg();
	const FSensorHistogram Flat0 = Flat(0.0f, 1);
	{
		FSensorController C;
		const FSensorFrameParams P = C.Update(Radiance(&Flat0, 1), Cfg);
		TestTrue(TEXT("default cap: finite, <= 40"), FMath::IsFinite(P.DisplayGain) && P.DisplayGain > 0.0f && P.DisplayGain <= 40.0f);
		TestTrue(TEXT("finite offset"), FMath::IsFinite(P.DisplayOffset));
	}
	{
		Cfg.AGCMaxDisplayGain = 5.0f;   // binds for a one-bin band
		FSensorController C;
		const FSensorFrameParams P = C.Update(Radiance(&Flat0, 1), Cfg);
		TestEqual(TEXT("cap binds"), P.DisplayGain, 5.0f);
		const int32 B = FSensorHistogram::BinOf(1.0f);
		const float N = P.PhotonGain * P.AnalogGain;
		const float Lo = FMath::Exp2(FSensorHistogram::MinLog2 + (B + 0.01f) / FSensorHistogram::BinsPerStop) * N;
		const float Hi = FMath::Exp2(FSensorHistogram::MinLog2 + (B + 0.99f) / FSensorHistogram::BinsPerStop) * N;
		TestEqual(TEXT("capped band centred on mid-grey"), 0.5f * (Lo + Hi) * P.DisplayGain + P.DisplayOffset, 0.5f, 1e-4f);
	}
	{
		Cfg.AGCMaxDisplayGain = 40.0f;   // bimodal: half land (bin B), half water (bin B + 2)
		FSensorController C;
		FSensorHistogram H;
		const int32 B = FSensorHistogram::BinOf(1.0f);
		H.Bins[B] = 5000; H.Bins[B + 2] = 5000; H.Serial = 1;
		const FSensorFrameParams P = C.Update(Radiance(&H, 1), Cfg);
		const float N = P.PhotonGain * P.AnalogGain;
		const float Land = FMath::Exp2(FSensorHistogram::BinCentreLog2(B)) * N * P.DisplayGain + P.DisplayOffset;
		TestTrue(*FString::Printf(TEXT("the lower mode is not black (v = %.3f)"), Land), Land > 0.05f);
	}
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile errors (`bRadianceInput`, `PercentileLog2Interp`, `ThermalAgcMargin`, the two-argument `TotalGainCapEv` do not exist).

- [ ] **Step 3: Implement.** In `Sensor/SensorController.h`, add to `FSensorControllerInput` after `FrameRateHz`:

```cpp
	/** ROADMAP 4A: IR mode with the thermal radiance input (signal = L / B(300 K)). The AE uses ThermalExposure and its
	 *  own state; the AGC interpolates percentiles, keeps a margin and is capped at AGCMaxDisplayGain. */
	bool                    bRadianceInput = false;
```

In `FSensorController` (public), after `ClipLinear`:

```cpp
	/** Thermal AE seed: a 300 K scene (signal ~1) at half full scale. */
	static constexpr float RadianceSeedGainEv = -1.0f;
	/** The thermal AGC maps its percentile band onto [margin, 1 - margin], so the pixels at the percentiles (half the frame in a
	 *  two-level night scene) are not black or white. */
	static constexpr float ThermalAgcMargin = 0.1f;
```

replace the `TotalGainCapEv` declaration with:

```cpp
	/** Highest total gain (EV): E.MaxPhotonGainEv + the analog stage's MaxAnalogGainDb in EV (none for a microbolometer). */
	static float TotalGainCapEv(const FSensorModeConfig& Cfg, const FSensorExposureConfig& E);
	/** TotalGainCapEv(Cfg, Cfg.Exposure). */
	static float TotalGainCapEv(const FSensorModeConfig& Cfg) { return TotalGainCapEv(Cfg, Cfg.Exposure); }
```

add after `PercentileLog2`:

```cpp
	/** Percentile P of H as log2 signal, interpolated linearly inside the bin (counts uniform across it); false when empty. */
	static bool PercentileLog2Interp(const FSensorHistogram& H, float P, float& OutLog2);
```

and in the private section replace `ESensorGraphMode LastMode = ESensorGraphMode::EO;` and the `GainEvByMode` line with:

```cpp
	/** AE state slot: 0 EO, 1 IR luminance proxy, 2 IR thermal radiance. */
	static int32 SlotOf(const FSensorControllerInput& In);
	static const FSensorExposureConfig& ExposureOf(const FSensorModeConfig& Cfg, const FSensorControllerInput& In);
	int32  LastSlot        = 0;
	/** AE loop state (total gain EV), one per slot: an excursion into another waveband or input never
	 *  disturbs this one's converged exposure. */
	float  GainEvByMode[3] = { -12.0f, -12.0f, RadianceSeedGainEv };
```

In `Sensor/SensorController.cpp`:

Add after `PercentileLog2`:

```cpp
bool FSensorController::PercentileLog2Interp(const FSensorHistogram& H, float P, float& OutLog2)
{
	const uint64 Total = H.Total();
	if (Total == 0) return false;
	const double Target = FMath::Clamp(static_cast<double>(P), 0.0, 1.0) * static_cast<double>(Total);
	uint64 Cum = 0;
	for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
	{
		const uint64 Next = Cum + H.Bins[B];
		if (H.Bins[B] > 0 && static_cast<double>(Next) >= Target)
		{
			const double F = FMath::Clamp((Target - static_cast<double>(Cum)) / static_cast<double>(H.Bins[B]), 0.0, 1.0);
			OutLog2 = FSensorHistogram::MinLog2 + static_cast<float>((B + F) / FSensorHistogram::BinsPerStop);
			return true;
		}
		Cum = Next;
	}
	OutLog2 = FSensorHistogram::MinLog2 + FSensorHistogram::NumBins / FSensorHistogram::BinsPerStop;
	return true;
}
```

Replace the anonymous-namespace `ModeIndex` users and add the slot helpers (keep `ModeIndex` for `ModeSeed`):

```cpp
int32 FSensorController::SlotOf(const FSensorControllerInput& In)
{
	if (In.Mode == ESensorGraphMode::EO) return 0;
	return In.bRadianceInput ? 2 : 1;
}

const FSensorExposureConfig& FSensorController::ExposureOf(const FSensorModeConfig& Cfg, const FSensorControllerInput& In)
{
	return SlotOf(In) == 2 ? Cfg.ThermalExposure : Cfg.Exposure;
}
```

Replace `TotalGainCapEv`:

```cpp
float FSensorController::TotalGainCapEv(const FSensorModeConfig& Cfg, const FSensorExposureConfig& E)
{
	const bool bAnalogStage = Cfg.Detector.Type != ESensorDetectorType::Microbolometer;
	const float AnalogEv = bAnalogStage ? FMath::Max(Cfg.Detector.MaxAnalogGainDb, 0.0f) / 20.0f * FMath::Log2(10.0f) : 0.0f;
	return E.MaxPhotonGainEv + AnalogEv;
}
```

In `UpdateAe`, replace `const FSensorExposureConfig& E = Cfg.Exposure;` with `const FSensorExposureConfig& E = ExposureOf(Cfg, In);`, the clamp line with `Target = FMath::Clamp(Target, E.MinGainEv, TotalGainCapEv(Cfg, E));`, and `float& GainEv = GainEvByMode[ModeIndex(In.Mode)];` with `float& GainEv = GainEvByMode[SlotOf(In)];`.

Replace `UpdateIrAgc` with:

```cpp
void FSensorController::UpdateIrAgc(const FSensorHistogram& H, const FSensorModeConfig& Cfg,
	const FSensorControllerInput& In, bool bSnap)
{
	float Lo = 0.0f, Hi = 0.0f, Median = 0.0f;
	// Thermal radiance has a large offset and a small contrast: interpolated percentiles keep the stretch from jumping a
	// whole 9 % bin at a time.
	auto Percentile = [&](float P, float& Out) { return In.bRadianceInput ? PercentileLog2Interp(H, P, Out) : PercentileLog2(H, P, Out); };
	if (!Percentile(Cfg.AGCLowPercentile, Lo) || !Percentile(Cfg.AGCHighPercentile, Hi)) return;
	if (PercentileLog2(H, 0.5f, Median)) LastMedianLog2 = Median;
	Hi = FMath::Max(Hi, Lo + (In.bRadianceInput ? 1e-3f : 1.0f / FSensorHistogram::BinsPerStop));  // never a zero-width band
	const float A = Smoothing(In.DeltaSimSec, Cfg.AGCLagFrames, bSnap);
	IrLoLog2 += (Lo - IrLoLog2) * A;
	IrHiLog2 += (Hi - IrHiLog2) * A;
}
```

In `Update`, replace the snap trigger:

```cpp
	const int32 Slot = SlotOf(In);
	if (Slot != LastSlot || In.bCameraCut)
	{
		bSnapPending = true;
		SnapAfterSerial = In.Serial;
		LastSlot = Slot;
	}
```

replace `const FSensorExposureConfig& E = Cfg.Exposure;` and the `TotalEv` line in the gain split with:

```cpp
	const FSensorExposureConfig& E = ExposureOf(Cfg, In);
	const float TotalEv  = FMath::Clamp(GainEvByMode[Slot], E.MinGainEv, TotalGainCapEv(Cfg, E));
```

and the `if (bIrAgc) { … }` display block with:

```cpp
	if (bIrAgc)
	{
		// Percentile band in signal units -> normalised DN, then stretched to [0, 1] (luminance proxy) or to
		// [margin, 1 - margin] with the gain capped and the band centred when the cap binds (thermal radiance).
		const float N    = P.PhotonGain * P.AnalogGain;
		const float LoN  = FMath::Exp2(IrLoLog2) * N;
		const float HiN  = FMath::Max(FMath::Exp2(IrHiLog2) * N, LoN + 1e-6f);
		if (In.bRadianceInput)
		{
			const float Span = 1.0f - 2.0f * ThermalAgcMargin;
			const float Gain = Span / (HiN - LoN);
			const float Cap  = FMath::Max(Cfg.AGCMaxDisplayGain, 1.0f);
			if (Gain > Cap)
			{
				P.DisplayGain   = Cap;
				P.DisplayOffset = 0.5f - 0.5f * (LoN + HiN) * Cap;
			}
			else
			{
				P.DisplayGain   = Gain;
				P.DisplayOffset = ThermalAgcMargin - LoN * Gain;
			}
		}
		else
		{
			P.DisplayGain   = 1.0f / (HiN - LoN);
			P.DisplayOffset = -LoN * P.DisplayGain;
		}
		LastEmittedGainEv = FMath::Log2(N * P.DisplayGain);
	}
```

(`LastMode` is no longer used; `ModeIndex` stays for `ModeSeed`.)

- [ ] **Step 4: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Sensor` → Expected: 0 failed, including the 4 new `CamSim.Sensor.Controller.*` tests and every existing one (`IrAgcZeroWidthBand`, `CutAndModeSwitchSnap`, `IrAgcToEoKeepsAeState` unchanged: the luminance path is untouched).
- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorController.h \
  unreal_project/CamSimTest/Source/CamSimTest/Sensor/SensorController.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/SensorControllerTest.cpp
git commit -F - <<'EOF'
feat(thermal): thermal AE slot, interpolated percentiles and a capped AGC with margin for radiance input

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 11: Sensor graph radiance input

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimShaders/Public/SensorGraph.h`, `unreal_project/CamSimTest/Source/CamSimShaders/Private/SensorGraph.cpp`
- Test: modify `unreal_project/CamSimTest/Source/CamSimTest/Tests/SensorGpuTest.cpp`

**Interfaces:**
- Produces: `FSensorGraphInputs::bRadianceInput` (`bool`, default `false`): `SceneColor` is `ThermalCS` radiance (R32F, read as `(L, 0, 0)`); the `USE_VIEW_PREEXPOSURE` permutation is never used (only `FSensorFrameParams::InputScale` scales it) and `Bloom` is ignored.

- [ ] **Step 1: Write the failing GPU test.** In `Tests/SensorGpuTest.cpp`, split `RunAndCompare` at the line `const int32 N = OutSize.X * OutSize.Y;`: move that line and everything after it, verbatim, into a new function placed just above `RunAndCompare`:

```cpp
	/** NV12 within Y <= 1 / UV <= 2 DN and histograms within tolerance (the binding GPU-vs-reference criteria). */
	void CompareResults(FAutomationTestBase& T, const FGpuResult& G, const CamSimSensorRef::FResult& R, FIntPoint OutSize,
		const TCHAR* Label)
	{
		// (the moved body of RunAndCompare, unchanged)
	}
```

and end `RunAndCompare` with `CompareResults(T, G, R, OutSize, Label);`. Then append to the file:

```cpp
// ---------------------------------------------------------------------------
// ROADMAP 4A: radiance input (ThermalCS output, R32F): no pre-exposure, no bloom, weights (1, 0, 0)
// ---------------------------------------------------------------------------

namespace
{
	FTextureRHIRef UploadR32(FRHICommandListImmediate& RHICmdList, const TArray<float>& Texels, FIntPoint Ext)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestRadiance"), Ext.X, Ext.Y, PF_R32_FLOAT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * sizeof(float),
			reinterpret_cast<const uint8*>(Texels.GetData()));
		return Tex;
	}

	FGpuResult RunRadianceOnGpu(const TArray<float>& Radiance, FIntPoint Size, const FGpuImage& Bloom, const FSensorFrameParams& P)
	{
		FGpuResult Result;
		ENQUEUE_RENDER_COMMAND(CamSimSensorRadianceGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef SceneTex = UploadR32(RHICmdList, Radiance, Size);
			FTextureRHIRef BloomTex = Upload(RHICmdList, Bloom, TEXT("CamSimTestBloom"));
			FRHIGPUBufferReadback Nv12Rb(TEXT("CamSimTestNv12"));
			FRHIGPUBufferReadback HistRb(TEXT("CamSimTestHist"));
			uint32 Nv12Bytes = 0;
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FSensorGraphInputs In;
				In.SceneColor = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(SceneTex, TEXT("CamSimTestRadiance")));
				In.SceneViewRect = FIntRect(FIntPoint::ZeroValue, Size);
				In.Bloom = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(BloomTex, TEXT("CamSimTestBloom")));
				In.BloomViewRect = Bloom.Rect;
				In.bRadianceInput = true;
				In.OutputSize = Size;
				const FSensorGraphOutputs Out = AddSensorPasses(GraphBuilder, In, P);
				Nv12Bytes = Out.Nv12Bytes;
				AddEnqueueCopyPass(GraphBuilder, &Nv12Rb, Out.Nv12, Nv12Bytes);
				AddEnqueueCopyPass(GraphBuilder, &HistRb, Out.Histogram, FSensorHistogram::NumBins * sizeof(uint32));
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuRadianceInputTest, "CamSim.GPU.Sensor.RadianceInputIgnoresBloom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuRadianceInputTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<float> Radiance;
	TArray<FLinearColor> AsRgb;
	for (int32 I = 0; I < W * H; ++I)
	{
		const float L = 1.5f + 0.5f * FMath::Sin(0.3f * (I % W)) * FMath::Cos(0.2f * (I / W));   // MWIR-like, W m^-2 sr^-1
		Radiance.Add(L);
		AsRgb.Add(FLinearColor(L, 0.0f, 0.0f, 1.0f));   // an R32F texel loads as (L, 0, 0)
	}
	TArray<FLinearColor> BloomTexels;
	BloomTexels.Init(FLinearColor(1e4f, 1e4f, 1e4f, 1.0f), W * H);   // would saturate everything if it were added
	FSensorFrameParams P = PresetParams(TEXT("mwir_cooled"), ESensorGraphMode::IR);
	P.SignalWeights = FVector3f(1.0f, 0.0f, 0.0f);
	P.InputScale = 0.5f;   // 1 / B(300 K) at runtime
	P.PhotonGain = 0.5f;
	P.DisplayGain = 6.0f;
	P.DisplayOffset = -2.0f;
	const CamSimSensorRef::FResult R = CamSimSensorRef::Run(AsRgb, W, H, P);   // reference: no bloom
	const FGpuResult G = RunRadianceOnGpu(Radiance, FIntPoint(W, H), FGpuImage::Whole(BloomTexels, W, H), P);
	CompareResults(*this, G, R, FIntPoint(W, H), TEXT("radiance input: "));
	return true;
}
```

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `bRadianceInput` is not a member of `FSensorGraphInputs`.

- [ ] **Step 3: Implement.** In `SensorGraph.h`, in `FSensorGraphInputs` after `OutputSize`:

```cpp
	/** ROADMAP 4A: SceneColor is ThermalCS radiance (R32F, loads as (L, 0, 0)). View pre-exposure is not divided out
	 *  (InputScale = 1 / B(300 K) scales it) and Bloom is ignored. */
	bool           bRadianceInput = false;
```

In `SensorGraph.cpp` `AddSensorPasses`, replace `const bool bUseView = In.ViewUniformBuffer != nullptr;` with:

```cpp
		const bool bUseView = In.ViewUniformBuffer != nullptr && !In.bRadianceInput;   // radiance has no pre-exposure
```

and `const bool bBloom = In.Bloom != nullptr && In.BloomViewRect.Width() > 0 && In.BloomViewRect.Height() > 0;` with:

```cpp
		const bool bBloom = !In.bRadianceInput && In.Bloom != nullptr && In.BloomViewRect.Width() > 0 && In.BloomViewRect.Height() > 0;
```

- [ ] **Step 4: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && scripts/run_gpu_tests.sh CamSim.GPU.Sensor` → Expected: every `CamSim.GPU.Sensor.*` passes, including `RadianceInputIgnoresBloom`.
- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimShaders/Public/SensorGraph.h \
  unreal_project/CamSimTest/Source/CamSimShaders/Private/SensorGraph.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/SensorGpuTest.cpp
git commit -F - <<'EOF'
feat(thermal): sensor graph radiance input — no pre-exposure, no bloom

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 12: Thermal GPU time in the frame stats

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/SensorGpuTimer.h`, `unreal_project/CamSimTest/Source/CamSimTest/Camera/SensorGpuTimer.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameStats.h`, `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameStats.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCaptureComponent.h`, `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp`
- Test: modify `unreal_project/CamSimTest/Source/CamSimTest/Tests/RenderPathTest.cpp` (`CamSim.Render.FrameStats.RowFormat`)

**Interfaces:**
- Produces:
  ```cpp
  struct FCamSimGpuStat final : public UE::RHI::GPUProfiler::FGPUStat   // replaces FCamSimSensorGpuStat
  {
      FCamSimGpuStat(const TCHAR* Name, const TCHAR* DisplayName);
      TAtomic<float> LatestMs { -1.0f };
  };
  extern FCamSimGpuStat GPUStat_CamSimSensor;    // RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimSensor, ...)
  extern FCamSimGpuStat GPUStat_CamSimThermal;   // RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimThermal, ...) (Task 14)
  float FSensorGpuTimer::GetThermalLatestMs() const;
  float FCamSimFrameStatsSample::ThermalGpuMs = -1.0f;   // JSON "thermal_gpu_ms"
  float UCamSimCaptureComponent::GetThermalGpuMs() const; // -1 unless the thermal pass ran last tick (bThermalActiveLastTick, set in Task 14)
  ```

- [ ] **Step 1: Write the failing test.** In `Tests/RenderPathTest.cpp` `FFrameStatsRowTest::RunTest`, set `S.ThermalGpuMs = 0.375f;` next to `S.SensorGpuMs = 1.25;`, add `TestEqual(TEXT("thermal_gpu_ms"), Obj->GetNumberField(TEXT("thermal_gpu_ms")), 0.375);` after the `sensor_gpu_ms` check, and in the legacy block add `S.ThermalGpuMs = -1.0f;` and `TestEqual(TEXT("legacy thermal_gpu_ms"), Legacy->GetNumberField(TEXT("thermal_gpu_ms")), -1.0);`.

- [ ] **Step 2: Run to verify failure** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5` → Expected: compile error, `ThermalGpuMs` is not a member of `FCamSimFrameStatsSample`.

- [ ] **Step 3: Implement.** Replace the stat struct and extern in `SensorGpuTimer.h`:

```cpp
/** One GPU profiler stat whose newest busy time is kept for the bench (CamSimSensor, CamSimThermal). */
struct FCamSimGpuStat final : public UE::RHI::GPUProfiler::FGPUStat
{
	FCamSimGpuStat(const TCHAR* InName, const TCHAR* InDisplayName) : FGPUStat(InName, InDisplayName, nullptr) {}
	virtual EOnTimingResultsAction OnTimingResults(UE::RHI::GPUProfiler::FQueue Queue, double BusyMs, double IdleMs, double WaitMs) override;
	/** Newest frame's busy time in ms; -1 until the first result. Any thread. */
	TAtomic<float> LatestMs { -1.0f };  // writer: GPU profiler thread (relaxed)
};

/** RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimSensor, ...) / (…, CamSimThermal, ...) resolve to these (HAS_GPU_STATS builds). */
extern FCamSimGpuStat GPUStat_CamSimSensor;
extern FCamSimGpuStat GPUStat_CamSimThermal;
```

and in `FSensorGpuTimer` add `float GetThermalLatestMs() const;` with the doc comment `/** ThermalCS's newest GPU time (ROADMAP 4A); -1 as GetLatestMs. Stale while the pass does not run: callers gate it (UCamSimCaptureComponent::GetThermalGpuMs). */`. In `SensorGpuTimer.cpp`:

```cpp
FCamSimGpuStat GPUStat_CamSimSensor(TEXT("CamSimSensor"), TEXT("CamSim sensor"));
FCamSimGpuStat GPUStat_CamSimThermal(TEXT("CamSimThermal"), TEXT("CamSim thermal"));

FCamSimGpuStat::EOnTimingResultsAction FCamSimGpuStat::OnTimingResults(
	UE::RHI::GPUProfiler::FQueue Queue, double BusyMs, double /*IdleMs*/, double /*WaitMs*/)
{
	if (Queue.Type == UE::RHI::GPUProfiler::FQueue::EType::Graphics && Queue.Index == 0)
	{
		LatestMs.Store(static_cast<float>(BusyMs), EMemoryOrder::Relaxed);
	}
	return EOnTimingResultsAction::Keep;
}

float FSensorGpuTimer::GetLatestMs() const
{
#if HAS_GPU_STATS
	return GPUStat_CamSimSensor.LatestMs.Load(EMemoryOrder::Relaxed);
#else
	return -1.0f;
#endif
}

float FSensorGpuTimer::GetThermalLatestMs() const
{
#if HAS_GPU_STATS
	return GPUStat_CamSimThermal.LatestMs.Load(EMemoryOrder::Relaxed);
#else
	return -1.0f;
#endif
}
```

In `CamSimFrameStats.h`, after `SensorGpuMs`: `float  ThermalGpuMs      = -1.0f;  // GPU time of ThermalCS, -1 when it did not run (ROADMAP 4A)`. In `CamSimFrameStats.cpp` `CamSimFormatFrameStatsRow`, change the last format line to `TEXT("\"sensor_gpu_ms\":%.3f,\"thermal_gpu_ms\":%.3f,\"sensor_gain_ev\":%s,\"scene_median_log2\":%s}"),` and pass `S.SensorGpuMs, S.ThermalGpuMs, *Num(S.SensorGainEv), *Num(S.SceneMedianLog2));`.

In `CamSimCaptureComponent.h`, after `GetSensorGpuMs()`:

```cpp
	/** GPU time of ThermalCS in ms; -1 unless the thermal pass ran last tick (ROADMAP 4A). */
	float GetThermalGpuMs() const { return bThermalActiveLastTick ? GpuTimer.GetThermalLatestMs() : -1.0f; }
```

and in the private 3B section after `ParamsSerial`: `/** The thermal pass was requested last tick (IR, thermal available and enabled, inputs gathered). */ bool bThermalActiveLastTick = false;`. In `CamSimCamera.cpp` `RecordFrameStats`, after `S.SensorGpuMs = …;`: `S.ThermalGpuMs     = CaptureComp->GetThermalGpuMs();`.

Check `scripts/bench/analyze.py` reads rows by key (it does: `r.get("sensor_gpu_ms", -1.0)`), so the added key needs no bench change.

- [ ] **Step 4: Run tests** — Run: `set -o pipefail; scripts/run.sh --build-only --mode editor 2>&1 | tail -5 && run_tests CamSim.Render` → Expected: 0 failed (`RowFormat` passes with the new key). Then `uv run -q --with pytest --with numpy --with pillow python -m pytest scripts/tests -q` → Expected: all pass.
- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/SensorGpuTimer.h \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/SensorGpuTimer.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameStats.h \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameStats.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCaptureComponent.h \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/RenderPathTest.cpp
git commit -F - <<'EOF'
feat(thermal): ThermalCS GPU stat and thermal_gpu_ms in the frame stats

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---


### Task 13: Frame sources — environment, entities, sun, subsystem gates (`ThermalFrameSources`)

Everything `FThermalFrameBuilder::Build` needs that lives in the running world: the CIGI atmosphere (air temperature, cloud cover, fog), the camera's geodetic pose, the ocean, the atmosphere sun light's illuminance (`K_lum`), and the live stencil → entity-type list. Pure helpers are unit-tested; the world-reading glue is a thin wrapper.

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameSources.h`, `unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameSources.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Environment/CamSimEnvironment.h`, `.cpp` (snapshot fields, `FoldAtmosphere`, `FoldWeather`)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Entity/CamSimEntity.h` (`IsSurfaceVehicle`), `Entity/CamSimEntityManager.h`, `.cpp` (`GetThermalStencilEntities`, tagging gate)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h`, `.cpp` (`IsThermalAvailable`, `IsEntityStencilTaggingEnabled`, fallback warning)
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalSourcesTest.cpp`

**Interfaces:**
- Consumes: `FThermalStencilEntity`, `FThermalFrameInputs` (Task 9); `FEntityTypeEntry::{ThermalMaterial, ThermalOffsetK}` (Task 4); `CamSimThermalPass::{bBaseColorAtTonemapper, bBaseColorSrgbEncoded}`, `IsThermalPassSupported` (Task 8); existing `FOceanSurface::{SeaLevelM, GetWaterTempC, GetWaves}`, `FOceanWaves::Amplitude(int32)`, `FCigiAtmosphereState`, `FCigiWeatherState::Coverage` (percent), `UCamSimSubsystem::{IsGroundTruthMaskAvailable, IsSensorGraphAvailable, GetOceanSurface, GetEntityManager}`.
- Produces:
  ```cpp
  // ACamSimEnvironment::FAtmosphericSnapshot — new fields
  float CloudCover01 = 0.0f;   // CIGI Weather Control coverage / 100, clamped (0 with no weather)
  bool  bFogActive   = false;  // Atmosphere visibility < 10 km or weather fog layer enabled
  // AirTempCelsius now really carries CIGI Atmosphere Control's air temperature (was never written)
  static void ACamSimEnvironment::FoldAtmosphere(FAtmosphericSnapshot& S, const FCigiAtmosphereState& A);
  static void ACamSimEnvironment::FoldWeather(FAtmosphericSnapshot& S, const FCigiWeatherState& W);

  // ACamSimEntity
  bool IsSurfaceVehicle() const;   // DIS domain land (1) or surface (3), or entity_category truck/boat/ground/sea

  // FCamSimEntityManager
  void GetThermalStencilEntities(TArray<FThermalStencilEntity>& Out) const;   // live, stencil != 0, rebuilt per call

  // UCamSimSubsystem
  bool IsThermalAvailable() const;              // sensor graph + IsThermalPassSupported + thermal.enabled; decided once
  bool IsEntityStencilTaggingEnabled() const;   // IsGroundTruthMaskAvailable() || IsThermalAvailable()

  namespace CamSimThermal
  {
      /** Illuminance (lux) on a horizontal surface from the atmosphere sun light: intensity × lum(colour)
       *  × lum(ground transmittance) × max(sin elevation, 0). 0 when no light / below horizon. */
      double SunIlluminanceLux(double IntensityLux, const FLinearColor& Color, const FLinearColor& Transmittance, double SunElevationDeg);
      /** Max wave amplitude (sum of component amplitudes, m) — the water band's margin. */
      double MaxWaveAmplitudeM(const FOceanWaves& Waves);
      /** World → FThermalFrameInputs (game thread). Fields it cannot read keep their defaults. */
      void GatherFrameInputs(UWorld* World, const UCamSimSubsystem& Subsystem, double CamLatDeg, double CamLonDeg,
                             double CamAltHaeM, const FVector& UpWorld, FThermalFrameInputs& Out);
  }
  ```

- [ ] **Step 1: Write the failing tests** — `Tests/ThermalSourcesTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Environment/CamSimEnvironment.h"
#include "CIGI/CigiPacketTypes.h"
#include "Ocean/OceanWaves.h"
#include "Thermal/ThermalFrameSources.h"

// CamSim.Thermal.Sources.*: world → FThermalFrameInputs helpers (ROADMAP 4A).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesAtmosphereTest, "CamSim.Thermal.Sources.AtmosphereFold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesAtmosphereTest::RunTest(const FString& Parameters)
{
	ACamSimEnvironment::FAtmosphericSnapshot S;
	FCigiAtmosphereState A;
	A.AirTemp = 31.5f; A.Visibility = 2500.0f; A.Humidity = 80;
	ACamSimEnvironment::FoldAtmosphere(S, A);
	TestEqual(TEXT("air temperature carried"), S.AirTempCelsius, 31.5f);
	TestEqual(TEXT("visibility carried"), S.AtmosphericVisibilityM, 2500.0f);
	TestTrue(TEXT("2.5 km visibility is fog"), S.bFogActive);
	A.Visibility = 40000.0f;
	ACamSimEnvironment::FoldAtmosphere(S, A);
	TestFalse(TEXT("40 km is not fog"), S.bFogActive);
	A.AirTemp = std::numeric_limits<float>::quiet_NaN();
	ACamSimEnvironment::FoldAtmosphere(S, A);
	TestEqual(TEXT("NaN air temperature ignored"), S.AirTempCelsius, 31.5f);

	FCigiWeatherState W;
	W.Coverage = 75.0f;
	ACamSimEnvironment::FoldWeather(S, W);
	TestEqual(TEXT("coverage 75 % -> 0.75"), S.CloudCover01, 0.75f);
	W.Coverage = 250.0f;
	ACamSimEnvironment::FoldWeather(S, W);
	TestEqual(TEXT("coverage clamped"), S.CloudCover01, 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesSunTest, "CamSim.Thermal.Sources.SunIlluminance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesSunTest::RunTest(const FString& Parameters)
{
	const FLinearColor White = FLinearColor::White;
	TestNearlyEqual(TEXT("zenith white sun = intensity"), CamSimThermal::SunIlluminanceLux(100000.0, White, White, 90.0), 100000.0, 1.0);
	TestNearlyEqual(TEXT("30 deg = half"), CamSimThermal::SunIlluminanceLux(100000.0, White, White, 30.0), 50000.0, 1.0);
	TestEqual(TEXT("below horizon = 0"), CamSimThermal::SunIlluminanceLux(100000.0, White, White, -5.0), 0.0);
	const FLinearColor Half(0.5f, 0.5f, 0.5f);
	TestNearlyEqual(TEXT("transmittance scales"), CamSimThermal::SunIlluminanceLux(100000.0, White, Half, 90.0), 50000.0, 1.0);
	TestEqual(TEXT("NaN intensity = 0"), CamSimThermal::SunIlluminanceLux(std::numeric_limits<double>::quiet_NaN(), White, White, 45.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesWaveTest, "CamSim.Thermal.Sources.MaxWaveAmplitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesWaveTest::RunTest(const FString& Parameters)
{
	FOceanWaves Calm;
	TestEqual(TEXT("no waves = 0"), CamSimThermal::MaxWaveAmplitudeM(Calm), 0.0);
	FOceanWaves Rough;
	Rough.SetSeaState(6.0, 270.0, 0.5);   // existing FOceanWaves API (see Ocean/OceanWaves.h); Beaufort 6
	const double A = CamSimThermal::MaxWaveAmplitudeM(Rough);
	TestTrue(TEXT("Beaufort 6 amplitude in (0.5, 6) m"), A > 0.5 && A < 6.0);
	return true;
}
```

Before running, open `Ocean/OceanWaves.h` and `CIGI/CigiPacketTypes.h` and correct the three existing-API spellings the test assumes (`FOceanWaves::SetSeaState` or its real Beaufort setter, and the `FCigiAtmosphereState` field names for air temperature / visibility / humidity); keep the assertions.

- [ ] **Step 2: Run to verify failure** — build → Expected: compile errors (`ThermalFrameSources.h` not found, `FoldAtmosphere` not a member).

- [ ] **Step 3: Implement the environment fold.** In `CamSimEnvironment.h` add the two snapshot fields above and the two static declarations. In `CamSimEnvironment.cpp`:

```cpp
void ACamSimEnvironment::FoldAtmosphere(FAtmosphericSnapshot& S, const FCigiAtmosphereState& A)
{
	if (FMath::IsFinite(A.AirTemp))    S.AirTempCelsius = A.AirTemp;
	if (FMath::IsFinite(A.Visibility) && A.Visibility > 0.0f) S.AtmosphericVisibilityM = A.Visibility;
	S.RelativeHumidity = FMath::Clamp(A.Humidity / 100.0f, 0.0f, 1.0f);
	S.bFogActive = S.AtmosphericVisibilityM < 10000.0f;
}

void ACamSimEnvironment::FoldWeather(FAtmosphericSnapshot& S, const FCigiWeatherState& W)
{
	S.CloudCover01 = FMath::IsFinite(W.Coverage) ? FMath::Clamp(W.Coverage / 100.0f, 0.0f, 1.0f) : 0.0f;
}
```

Call `FoldAtmosphere(CachedAtmosSnapshot, CurrentAtmosphere)` where the atmosphere is applied (today the line `CachedAtmosSnapshot.AtmosphericVisibilityM = CurrentAtmosphere.Visibility;`, replace it) and `FoldWeather(CachedAtmosSnapshot, CurrentWeather)` where weather is applied (next to `const float Coverage01 = ...`). Before any CIGI atmosphere arrives, seed `CachedAtmosSnapshot.AirTempCelsius` in `BeginPlay` from `Cfg.Thermal.AirTemperatureC` (Task 3).

- [ ] **Step 4: Implement entities.** `ACamSimEntity::IsSurfaceVehicle()` returns true for DIS domain 1 or 3 (the same test `Entity/SurfaceClamp.h` uses to decide clamping — reuse its predicate, don't duplicate the numbers) and for CIGI/scenario entities whose type entry's `EntityCategory` is `ground` or `sea`. In `FCamSimEntityManager`:

```cpp
void FCamSimEntityManager::GetThermalStencilEntities(TArray<FThermalStencilEntity>& Out) const
{
	Out.Reset();
	for (const TPair<FEntityKey, uint8>& KV : StencilOf)
	{
		const ACamSimEntity* E = FindEntity(KV.Key);   // existing lookup; skip pending-kill
		if (!IsValid(E) || KV.Value == 0) continue;
		FThermalStencilEntity& T = Out.AddDefaulted_GetRef();
		T.Stencil = KV.Value;
		if (const FEntityTypeEntry* Type = E->GetTypeEntry())   // existing accessor for the entity's type row
		{
			T.ThermalMaterial = Type->ThermalMaterial;
			T.ThermalOffsetK  = Type->ThermalOffsetK;
		}
		T.bSurfaceVehicle = E->IsSurfaceVehicle();
	}
}
```

(use the manager's real entity-map lookup and the entity's real type accessor; `StencilOf` already holds exactly the live tagged entities, released 4 frames late by `FStencilSlotAllocator`, so a reused value always maps to its *current* owner). Change the tagging gate at `CamSimEntityManager.cpp` (the `if (Subsystem && Subsystem->IsGroundTruthMaskAvailable())` before stencil allocation) to `Subsystem->IsEntityStencilTaggingEnabled()`.

- [ ] **Step 5: Implement the subsystem gates.** In `UCamSimSubsystem::Initialize`, right after `bGroundTruthMaskAvailable` is decided:

```cpp
	bThermalAvailable = false;
	if (bSensorGraphAvailable && Cfg.Thermal.bEnabled)
	{
		FString Why;
		bThermalAvailable = IsThermalPassSupported(Why);
		if (!bThermalAvailable)
		{
			UE_LOG(LogCamSim, Warning, TEXT("Thermal: ThermalCS unavailable (%s); IR uses the visible-light proxy"), *Why);
		}
		else if (!CamSimThermalPass::bBaseColorAtTonemapper)
		{
			UE_LOG(LogCamSim, Warning, TEXT("Thermal: GBuffer base colour is not readable at the tonemapper (ROADMAP 4A spike); ")
				TEXT("the per-pixel solar term is off (k_fast = 0)"));
		}
	}
```

with `bool bThermalAvailable = false;` next to `bGroundTruthMaskAvailable` and the two inline getters from Interfaces.

- [ ] **Step 6: Implement `ThermalFrameSources.cpp`.** `SunIlluminanceLux`: return 0 for non-finite or ≤ 0 intensity or elevation ≤ 0; else `I * Lum(Color) * Lum(Tr) * sin(el)` with `Lum = 0.2126 R + 0.7152 G + 0.0722 B`. `MaxWaveAmplitudeM`: sum of `Waves.Amplitude(i)` over the wave count. `GatherFrameInputs`:

```cpp
void CamSimThermal::GatherFrameInputs(UWorld* World, const UCamSimSubsystem& Subsystem, double CamLatDeg, double CamLonDeg,
	double CamAltHaeM, const FVector& UpWorld, FThermalFrameInputs& Out)
{
	Out.UtcMicros = FSimClock::Get().NowMicros();
	Out.CamLatDeg = CamLatDeg; Out.CamLonDeg = CamLonDeg; Out.CamAltHaeM = CamAltHaeM;
	Out.UpWorld = UpWorld;
	Out.bBaseColorAvailable = CamSimThermalPass::bBaseColorAtTonemapper;
	Out.bBaseColorSrgb      = CamSimThermalPass::bBaseColorSrgbEncoded;
	double SunElevDeg = 0.0;
	if (TActorIterator<ACamSimEnvironment> It(World); It)
	{
		const ACamSimEnvironment::FAtmosphericSnapshot S = It->GetAtmosphericSnapshot();
		Out.AirTempC = S.AirTempCelsius; Out.CloudCover01 = S.CloudCover01;
		Out.VisibilityM = S.AtmosphericVisibilityM; Out.bFogActive = S.bFogActive;
		SunElevDeg = It->GetSunElevationDeg();
	}
	// Atmosphere sun light (Task 1 record): visible, used as atmosphere sun, index 0, brightest.
	const UDirectionalLightComponent* Sun = nullptr;
	for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
	{
		if (It->GetWorld() != World || !It->IsVisible() || !It->IsUsedAsAtmosphereSunLight() || It->GetAtmosphereSunLightIndex() != 0) continue;
		if (!Sun || It->Intensity > Sun->Intensity) Sun = *It;
	}
	if (Sun)
	{
		FLinearColor Tr = FLinearColor::White;
		for (TObjectIterator<USkyAtmosphereComponent> S; S; ++S)
		{
			if (S->GetWorld() == World) { Tr = S->GetAtmosphereTransmitanceOnGroundAtPlanetTop(const_cast<UDirectionalLightComponent*>(Sun)); break; }
		}
		Out.SunIlluminanceLux = SunIlluminanceLux(Sun->Intensity, Sun->GetLightColor(), Tr, SunElevDeg);
	}
	if (const FOceanSurface* Ocean = Subsystem.GetOceanSurface())
	{
		Out.WaterTempC = Ocean->GetWaterTempC();
		if (const TOptional<double> Sea = Ocean->SeaLevelM(CamLatDeg, CamLonDeg))
		{
			Out.bHasSea = true; Out.SeaLevelHaeM = *Sea; Out.MaxWaveAmplitudeM = MaxWaveAmplitudeM(Ocean->GetWaves());
		}
	}
	if (const FCamSimEntityManager* EM = Subsystem.GetEntityManager()) EM->GetThermalStencilEntities(Out.Entities);
}
```

Use whatever the Task 1 record says about the atmosphere sun light (if it named a different component/owner, select that one instead of the generic filter). Caching the two `TObjectIterator` lookups as `TWeakObjectPtr` statics refreshed every 300 frames is allowed (the builder budget is < 50 µs).

- [ ] **Step 7: Run tests** — build, `run_tests CamSim.Thermal` → Expected: 0 failed; then `run_tests CamSim` → Expected: 0 failed (the tagging-gate change must not alter ground-truth tests: with ML off and thermal unavailable under NullRHI nothing is tagged, as before).

- [ ] **Step 8: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Thermal/ThermalFrameSources.* \
  unreal_project/CamSimTest/Source/CamSimTest/Environment/CamSimEnvironment.* \
  unreal_project/CamSimTest/Source/CamSimTest/Entity/CamSimEntity.h \
  unreal_project/CamSimTest/Source/CamSimTest/Entity/CamSimEntity.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Entity/CamSimEntityManager.* \
  unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.* \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalSourcesTest.cpp
git commit -F - <<'EOF'
feat(thermal): frame sources — CIGI air temperature/cloud/fog, sun illuminance, stencil entities, thermal gates

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 14: Wire the thermal path into the capture (IR mode)

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCaptureComponent.h`, `.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameGrabExtension.h`, `.cpp`
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp` (pass the camera's geodetic pose + up vector)
- Test: `unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalBuilderTest.cpp` (append `CamSim.Thermal.Builder.ModeGate`), GPU smoke `CamSim.GPU.Thermal.EndToEndRadianceToNv12` in `Tests/ThermalGpuTest.cpp`

**Interfaces:**
- Consumes: `FThermalFrameBuilder` (Task 9), `CamSimThermal::GatherFrameInputs` (Task 13), `UCamSimSubsystem::IsThermalAvailable` (Task 13), `AddThermalPass`, `FThermalPassInputs` (Task 8), `FSensorGraphInputs::bRadianceInput` (Task 11), `FSensorControllerInput::bRadianceInput` (Task 10), `GPUStat_CamSimThermal`, `bThermalActiveLastTick` (Task 12).
- Produces:
  ```cpp
  // UCamSimCaptureComponent
  static constexpr float ThermalUeExposureEv = -12.0f;   // fixed UE AutoExposureBias in thermal mode (Decisions)
  static bool ShouldRunThermal(ESensorMode Mode, bool bThermalAvailable);   // Mode == IR && available
  void SetThermalPose(double LatDeg, double LonDeg, double AltHaeM, const FVector& UpWorld);   // camera, per tick
  // FCamSimFrameGrabExtension
  void SetThermalParams_RenderThread(TSharedPtr<const FThermalFrameParams, ESPMode::ThreadSafe> P);   // null = thermal off
  ```

- [ ] **Step 1: Write the failing tests.** Append to `Tests/ThermalBuilderTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModeGateTest, "CamSim.Thermal.Builder.ModeGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModeGateTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("IR + available"), UCamSimCaptureComponent::ShouldRunThermal(ESensorMode::IR, true));
	TestFalse(TEXT("EO never"), UCamSimCaptureComponent::ShouldRunThermal(ESensorMode::EO, true));
	TestFalse(TEXT("IR unavailable (thermal.enabled false / no shader)"), UCamSimCaptureComponent::ShouldRunThermal(ESensorMode::IR, false));
	return true;
}
```

(include `"Camera/CamSimCaptureComponent.h"`). Append to `Tests/ThermalGpuTest.cpp` a GPU test `CamSim.GPU.Thermal.EndToEndRadianceToNv12` that builds `CamSimThermalTest::MakeScene`, runs `AddThermalPass` and feeds its output into `AddSensorPasses` with `bRadianceInput = true` and `InputScale = 1 / B(300 K)` in one graph, and checks: the NV12 Y plane is finite, the sky region's mean Y is lower than the terrain region's (white-hot), and the entity region's mean Y is higher than terrain at night parameters. Reuse the scene-region rects `MakeScene` defines.

- [ ] **Step 2: Run to verify failure** — build → Expected: compile error, `ShouldRunThermal` is not a member.

- [ ] **Step 3: Game thread.** In `UCamSimCaptureComponent`:
  - members: `FThermalFrameBuilder ThermalBuilder; bool bThermalConfigured = false; ESensorMode ThermalBandMode; double ThermalLat = 0, ThermalLon = 0, ThermalAlt = 0; FVector ThermalUp = FVector::UpVector; bool bThermalActiveLastTick = false;` (the last one from Task 12 if already declared).
  - `ShouldRunThermal`: `return Mode == ESensorMode::IR && bThermalAvailable;`
  - In `UpdateSensorParams`, before building `FSensorControllerInput`:

```cpp
	const bool bThermal = ShouldRunThermal(Mode, Subsystem && Subsystem->IsThermalAvailable());
	TSharedPtr<FThermalFrameParams, ESPMode::ThreadSafe> ThermalParams;
	if (bThermal)
	{
		if (!bThermalConfigured)
		{
			ThermalBuilder.Configure(Cfg.Thermal, MC.Detector.BandLoUm, MC.Detector.BandHiUm);
			bThermalConfigured = true;
		}
		FThermalFrameInputs TIn;
		CamSimThermal::GatherFrameInputs(GetWorld(), *Subsystem, ThermalLat, ThermalLon, ThermalAlt, ThermalUp, TIn);
		ThermalParams = MakeShared<FThermalFrameParams, ESPMode::ThreadSafe>();
		TArray<FString> Warnings;
		ThermalBuilder.Build(TIn, *ThermalParams, &Warnings);
		for (const FString& W : Warnings) UE_LOG(LogCamSim, Warning, TEXT("Thermal: %s"), *W);
	}
	bThermalActiveLastTick = bThermal;
```

    (`MC` must be resolved before this block — move the `ModeCfg`/`MC` lines up; reset `bThermalConfigured = false` whenever `Cfg`'s IR band changes, e.g. on hot reload, by comparing `ThermalBuilder.GetBand().GetLoUm()/GetHiUm()` against `MC.Detector.BandLoUm/HiUm`.)
  - `In.bRadianceInput = bThermal;` on the controller input. After `SensorController.Update`, when `bThermal`: `Params.SignalWeights = FVector3f(1, 0, 0); Params.InputScale = ThermalBuilder.GetSignalScale();`.
  - UE exposure: `Sensor->PostProcessSettings.AutoExposureBias = bThermal ? ThermalUeExposureEv : SensorController.GetGainEv() + UeExposureOffsetEv;`
  - Enqueue both: the existing `SetParams_RenderThread(Params)` and `Ext->SetThermalParams_RenderThread(ThermalParams)` in the same render command (so a frame never pairs IR thermal params with EO sensor params).
  - `SetThermalPose` stores the four values. In `CamSimCamera.cpp`, call it each tick before `UpdateSensorParams` with the camera's geodetic position (the same lat/lon/HAE the telemetry assembler uses for KLV Tags 13/14/75) and the local up vector in UE world space (`GlobeAnchor`'s East-South-Up → up = the georeference's `ComputeEastSouthUpToUnrealTransformation(...)` Z axis at the camera, normalised).

- [ ] **Step 4: Render thread.** In `FCamSimFrameGrabExtension`: member `TSharedPtr<const FThermalFrameParams, ESPMode::ThreadSafe> ThermalParams; bool bWarnedThermalInputs = false;`. In `RunSensor_RenderThread`, after `In` is filled and before `AddSensorPasses`:

```cpp
	if (ThermalParams.IsValid())
	{
		const FSceneTextureUniformParameters* St = Inputs.SceneTextures.SceneTextures
			? Inputs.SceneTextures.SceneTextures->GetParameters().GetContents() : nullptr;
		if (St && St->SceneDepthTexture && St->CustomDepthTexture && St->CustomStencilTexture)
		{
			FThermalPassInputs Ti;
			Ti.SceneColor        = SceneColor.Texture;
			Ti.SceneColorRect    = SceneColor.ViewRect;
			Ti.SceneDepth        = St->SceneDepthTexture;
			Ti.CustomDepth       = St->CustomDepthTexture;
			Ti.CustomStencil     = St->CustomStencilTexture;
			Ti.BaseColor         = CamSimThermalPass::bBaseColorAtTonemapper ? St->GBufferCTexture : nullptr;
			Ti.ViewUniformBuffer = View.ViewUniformBuffer.GetReference();
			FRDGTextureRef Radiance;
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimThermal, "CamSimThermal");
				Radiance = AddThermalPass(GraphBuilder, Ti, *ThermalParams);
			}
			In.SceneColor     = Radiance;
			In.SceneViewRect  = FIntRect(FIntPoint::ZeroValue, SceneColor.ViewRect.Size());
			In.Bloom          = nullptr;
			In.bRadianceInput = true;
		}
		else if (!bWarnedThermalInputs)
		{
			bWarnedThermalInputs = true;
			UE_LOG(LogCamSim, Warning, TEXT("Thermal: the post-process inputs carry no depth / custom depth / stencil; ")
				TEXT("IR falls back to the visible-light proxy this session (logged once)"));
		}
	}
```

If the inputs are missing, the frame still runs with the luminance input; the controller sees `bRadianceInput` mismatching only for that frame (acceptable: the warning makes it visible and the condition does not occur on a deferred renderer). Ground truth (`AddInstanceIdReadback_RenderThread`) is unchanged.

- [ ] **Step 5: Run tests** — build; `run_tests CamSim` → Expected: 0 failed. `scripts/run_gpu_tests.sh CamSim.GPU` → Expected: all pass, including `CamSim.GPU.Thermal.*` and the unchanged `CamSim.GPU.Sensor.*`.

- [ ] **Step 6: Live smoke.** `CAMSIM_MULTICAST_ADDR=127.0.0.1 scripts/run.sh --headless`, drive it with `scripts/send_cigi_test.py --circle` and switch to IR (CIGI Sensor Control, or `sensor_modes` default via `CAMSIM_SENSOR_MODE=ir` if that env var exists — check `docs/configuration.md`). Expected in the log: no `Thermal:` warnings except the documented spike fallback; `/metrics` and the bench row show `thermal_gpu_ms` > 0. Capture one frame with `ffmpeg -i udp://127.0.0.1:5000 -frames:v 1 $TMPDIR/ir.png` (port from config) and look at it: sky dark, ground mid-grey, no NaN speckle.

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCaptureComponent.* \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimFrameGrabExtension.* \
  unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalBuilderTest.cpp \
  unreal_project/CamSimTest/Source/CamSimTest/Tests/ThermalGpuTest.cpp
git commit -F - <<'EOF'
feat(thermal): IR renders thermal radiance — ThermalCS before SensorCS, radiance AE, fixed UE exposure

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 15: Live acceptance — `scripts/thermal_check.py`

**Files:**
- Create: `scripts/thermal_check.py`
- Modify: `CLAUDE.md` (command table row)

**Interfaces:**
- Consumes: `scripts/dis_vehicle_check.py` (`nadir_on`, `TRUCK`, `TRUCK_GROUND_HAE`, boat helpers), `scripts/bench/run_bench.py` (`Host`, `wait_ready`, `wait_terrain`, frame-stats rows), `scripts/send_dis_test.py` (truck + boat), `scripts/gt_occlusion_check.py` (COCO reading, shot saving, report layout — copy its structure), env `CAMSIM_IR_PRESET`, `CAMSIM_CAPTURE_WIDTH/HEIGHT` (Task 4).
- Produces: `uv run scripts/thermal_check.py [--band mwir|lwir|both] [--out DIR]` → exit 0 iff all checks pass; writes shots, a `report.md` and `report.json` under `--out` (default `.cache/thermal_check/<timestamp>`).

- [ ] **Step 1: Write the script.** PEP 723 header (`requires-python = ">=3.10"`, `numpy`, `pillow`). Structure:
  1. For each band in `--band` (default both: `mwir_cooled`, `lwir_uncooled` via `CAMSIM_IR_PRESET`), and each time in {21 Dec solar noon = 20:10 UTC, 02:00 local = 10:10 UTC}: launch CamSim headless (`--local --detach`, `CAMSIM_MULTICAST_ADDR=127.0.0.1`, ML ground truth on so COCO boxes exist), start the DIS truck + boat (`send_dis_test.py both`), host pose = nadir over the truck at 330 m with 30° FOV (as Task 1), switch to IR, wait ready + terrain + 10 s for AE/AGC, capture 30 frames (NV12 Y → PNG for the middle one), read COCO for the truck/boat boxes, then stop.
  2. A second pose per run for check (e): 90° FOV, pitched +20° above the horizon, same position.
  3. A third pose for check (d) at noon: oblique 45° over the truck so its shadow is in frame; the shadow region is the truck box shifted along the sun azimuth by its height / tan(28.8°) (computed from the sun azimuth in the log line `sun elevation`/`azimuth`, or `ACamSimEnvironment::ComputeSunPosition` mirrored in Python).
  4. A 1080p run (`CAMSIM_CAPTURE_WIDTH=1920`, `HEIGHT=1080`) at noon MWIR for check (f), reading `thermal_gpu_ms` p95 from the frame-stats rows.
  5. An EO run with `thermal.enabled` default, comparing its mean Y and frame time against an EO run with `CAMSIM_THERMAL_ENABLED=0` for check (g) (|ΔY| ≤ 1 DN mean: thermal must not touch EO).
- Checks (thresholds from the spec):
  - (a) night IR: mean Y in [60, 180], < 5 % of pixels at Y ≤ 16.
  - (b) night: truck box mean Y ≥ terrain ring (box dilated 2×, minus the box) mean + 3 DN (white-hot).
  - (c) water vs land: (boat-free water patch − land ring) mean Y sign at noon is opposite to the sign at night.
  - (d) noon: shadow region mean Y < sunlit adjacent ground mean Y.
  - (e) sky region (top 30 % of the pitched-up frame) mean Y < terrain region (bottom 30 %) mean Y, both bands, night and noon.
  - (f) `thermal_gpu_ms` p95 ≤ 0.5 at 1080p.
  - (g) EO unchanged by thermal (as above).
  - Edge shimmer (spec risk): temporal std of Y on the truck-box boundary pixels over the 30 frames ≤ 2 × the std of interior terrain pixels; report-only (not a gate) unless it fails visibly, in which case record it in ROADMAP as the 3×3 class-vote follow-up.
- Report: one table row per check with value, threshold, pass/fail; shot paths.

- [ ] **Step 2: Dry-run the script's pure parts.** Put the region/statistics helpers in functions and add `scripts/tests/test_thermal_check.py` (pytest) covering: ring mask = dilated box minus box, shadow-offset geometry at 28.8° elevation, percentile/mean on a synthetic Y image, and report JSON shape. Run: `uv run -q --with pytest --with numpy --with pillow python -m pytest scripts/tests/test_thermal_check.py -q` → Expected: pass.

- [ ] **Step 3: Run it live** — `uv run scripts/thermal_check.py --band both` → Expected: all gates pass. If a gate fails, debug with superpowers:systematic-debugging before touching thresholds; thresholds change only with a ROADMAP note saying why.

- [ ] **Step 4: CLAUDE.md row** (Commands table, after `gt_occlusion_check.py`):

```markdown
| `scripts/thermal_check.py` | Thermal IR acceptance (ROADMAP 4A): MWIR/LWIR at noon and 02:00 with the DIS truck + boat — night IR level, white-hot vehicle, water/land crossover, cool shadows, cold sky, ThermalCS time, EO untouched |
```

- [ ] **Step 5: Commit**

```bash
git add scripts/thermal_check.py scripts/tests/test_thermal_check.py CLAUDE.md
git commit -F - <<'EOF'
test(thermal): thermal_check.py live acceptance (ROADMAP 4A checks a-g)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

---

### Task 16: Docs — ROADMAP 4A results, guide, CLAUDE.md

**Files:**
- Modify: `ROADMAP.md` (4A section: status, results table from `report.md`, known issues, carry-overs to 4B/4C/4D; "no editor/human changes needed" stated explicitly; strike 3B.3's "AGC max gain cap" as done here)
- Modify/Create: `docs/thermal.md` (created in Task 3 — complete it: model, classes, config keys, how to add a material, how entity types pick one, the fast term and its fallback, performance, limits), `docs/architecture.md` (data-flow line for ThermalCS), `docs/configuration.md` (confirm every key added in Tasks 3/4 is present)
- Modify: `CLAUDE.md` — gotchas: replace "**IR is a visible-light proxy** until Milestone 4 …" with a **Thermal IR (ROADMAP 4A)** entry: `ThermalCS` runs before `SensorCS` in IR only; `CamSimThermalRef::EvaluatePixel` is its CPU mirror (change both, `CamSim.GPU.Thermal.MatchesCpu`); class temperatures are closed-form per class on the game thread (`FThermalModel`, no state, sim time); UE exposure is fixed at −12 EV in thermal mode; the per-pixel solar term reads GBuffer base colour (spike verdict); entity stencils are tagged when thermal is available, not only for ML; `thermal.enabled: false` restores the luminance proxy. Update the test count line (`Tests/` count and files) from the final `run_tests CamSim` result.

- [ ] **Step 1: Write the docs** from the actual results (numbers from `report.json`, GPU times from the bench rows, test counts from the last full run). No placeholders: every number is measured.
- [ ] **Step 2: Verify** — `run_tests CamSim` → 0 failed (update the count in CLAUDE.md to match); `scripts/run_gpu_tests.sh` → all pass; `scripts/ci_validate.sh --native` → passes (KLV/video unchanged).
- [ ] **Step 3: Commit**

```bash
git add ROADMAP.md docs/thermal.md docs/architecture.md docs/configuration.md CLAUDE.md
git commit -F - <<'EOF'
docs(thermal): ROADMAP 4A results, thermal guide, CLAUDE.md gotchas

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01E31DBL2SpYZzXra2gESRvQ
EOF
```

- [ ] **Step 4: Visual review hand-off** — send the user the MWIR/LWIR noon/night shot set (and the EO comparison) for visual review; ROADMAP 4A stays "awaiting visual review" until they sign off.
