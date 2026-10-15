# Control API (ROADMAP 2.4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An HTTP/JSON control API on CamSim's health port that sets up a scene, poses the camera, steps a lockstep
clock and captures a frame with its ground truth inline, plus a Python client that writes seeded COCO datasets.

**Architecture:** A game-thread `FControlApiService` binds `/api/v1/*` routes on `FCamSimHealthServer`. Requests
are parsed by pure functions into canonical commands (`Sim/Commands.h`). `FControlApiAdapter` holds API entities and
re-submits them to the entity manager each tick. Camera and environment commands go to new canonical entry points on
the rig, gimbal, sensor and environment. A capture runs a pure settle state machine (`FControlCaptureGate`), then
tags one capture frame. That frame's ground-truth record, PNG and depth PNG come back on an `FControlCaptureJob`
filled on the task thread.

**Tech Stack:** UE 5.8 C++ (HTTPServer, Json, ImageWrapper), UE Automation tests (NullRHI), Python 3.11+ standard
library (`uv` project, pytest, pycocotools only in the acceptance script).

**Spec:** `docs/superpowers/specs/2026-10-15-control-api-design.md`

## Global Constraints

- Copyright header `// Copyright CamSim Contributors. All Rights Reserved.`; `CoreMinimal.h` first; UE naming (F/U/A/E/I).
- Off by default: `control_api.enabled: false` (`CAMSIM_CONTROL_API_ENABLED`); needs `operational.health_http_enabled`.
- Routes under `/api/v1/`; JSON errors `{"error": "..."}` with 400 / 404 / 409 / 503.
- Heights are WGS-84 ellipsoid metres; heading/pitch/roll are CIGI's (`CamSimFrames::CigiToNeu`).
- Step limits: `0 < seconds <= 3600`. Entity ids 1..65535. `settle_load_pct` default 99.9, `default_settle_frames` 30,
  `capture_timeout_s` 120.
- Lockstep: sim delta for dead reckoning and gimbal slew; AE/AGC on frame time (1/`frame_rate`). Outside lockstep
  behaviour is unchanged.
- Never `FQuat::Rotator()` for a sensor orientation (CLAUDE.md gotcha); the camera path goes through the existing rig.
- Tests run headless: `"$UE_BIN" "$PWD/unreal_project/CamSimTest/CamSimTest.uproject" -ExecCmds="Automation RunTests CamSim.ControlApi+Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="$PWD/.cache/automation-report" -unattended -nullrhi -nosound -nosplash -log -stdout -FullStdOutLogOutput` from the repo root, after `scripts/run.sh --build-only`.

## Review Focus

1. **A capture requested while the camera is mid-move or tiles are still loading** must wait, not return a stale
   frame: the gate resets its frame count on any command (Task 5 test `ResetsOnCommand`).
2. **Two captures at once / a client that disconnects mid-capture**: the second gets 409; a timed-out capture
   completes its callback exactly once (Task 5 `TimesOutOnce`, Task 8 HTTP test `SecondCapture409`).
3. **Malformed or partial JSON** (missing lat with lon present, NaN, strings for numbers, unknown sensor mode) → 400
   with a message, never a crash or a half-applied command (Task 4 tests).
4. **Lockstep step with no listener of sim delta** (CIGI-driven DR entities in lockstep) must not move between steps,
   and a 0-delta frame must not divide by zero in AE (Task 1 `FSimDelta` tests, `SensorStepSeconds`).
5. **A forgotten CIGI host still sending camera packets** must not move an API-owned camera (Task 3 test of the
   camera suppression flag; acceptance C5).

---

### Task 1: Sim delta for lockstep (dead reckoning, gimbal slew, AE)

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Time/SimDelta.h`
- Modify: `Entity/CamSimEntity.h/.cpp` (member `FSimDelta DrDelta;`, `UpdateDeadReckoning(DrDelta.Next(DeltaTime))`)
- Modify: `Camera/CamSimCamera.h/.cpp` (member `FSimDelta GimbalDelta;`, `ApplyCigiViewState(GimbalDelta.Next(DeltaTime))`)
- Modify: `Camera/CamSimCaptureComponent.cpp` `UpdateSensorParams` (AE step via `CamSimTime::SensorStepSeconds`)
- Test: `Tests/SimDeltaTest.cpp`

**Interfaces:**
- Produces: `class FSimDelta { double Next(double EngineDeltaSec); explicit FSimDelta(FSimClock* Clock = nullptr); }`;
  `double CamSimTime::SensorStepSeconds(bool bLockstep, double SimDeltaSec, float FrameRate)`.

- [ ] **Step 1: Write the failing tests** (`Tests/SimDeltaTest.cpp`)

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimDeltaRealTimeTest, "CamSim.SimDelta.RealTimeUsesEngineDelta", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSimDeltaRealTimeTest::RunTest(const FString&)
{
	FSimClock Clock; double Mono = 0.0; Clock.SetMonotonicSource([&Mono] { return Mono; });
	FSimDelta D(&Clock);
	TestEqual(TEXT("real time: engine delta"), D.Next(1.0 / 30.0), 1.0 / 30.0);
	Clock.SetRate(0.0);
	TestEqual(TEXT("frozen clock outside lockstep: still the engine delta (fixed-sun hosts keep entities moving)"), D.Next(0.5), 0.5);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimDeltaLockstepTest, "CamSim.SimDelta.LockstepUsesClockSteps", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSimDeltaLockstepTest::RunTest(const FString&)
{
	FSimClock Clock; double Mono = 0.0; Clock.SetMonotonicSource([&Mono] { return Mono; });
	Clock.SetLockstep(true);
	FSimDelta D(&Clock);
	TestEqual(TEXT("first call: 0"), D.Next(1.0 / 30.0), 0.0);
	Mono += 5.0;
	TestEqual(TEXT("no step: 0 whatever the engine delta"), D.Next(1.0 / 30.0), 0.0);
	Clock.Step(2.5);
	TestTrue(TEXT("step: exactly the step"), FMath::IsNearlyEqual(D.Next(1.0 / 30.0), 2.5, 1e-6));
	TestEqual(TEXT("consumed"), D.Next(1.0 / 30.0), 0.0);
	Clock.SetUtc(FDateTime(2020, 1, 1));
	TestEqual(TEXT("a backward jump is 0, never negative"), D.Next(1.0 / 30.0), 0.0);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorStepTest, "CamSim.SimDelta.SensorStepSeconds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorStepTest::RunTest(const FString&)
{
	TestEqual(TEXT("real time: sim delta"), CamSimTime::SensorStepSeconds(false, 0.2, 30.f), 0.2);
	TestTrue(TEXT("lockstep: one frame"), FMath::IsNearlyEqual(CamSimTime::SensorStepSeconds(true, 0.0, 30.f), 1.0 / 30.0, 1e-9));
	TestTrue(TEXT("lockstep, bad frame rate: 1/30"), FMath::IsNearlyEqual(CamSimTime::SensorStepSeconds(true, 0.0, 0.f), 1.0 / 30.0, 1e-9));
	return true;
}
```

- [ ] **Step 2: Build; run `CamSim.SimDelta` → compile failure (header missing).**
- [ ] **Step 3: Implement `Time/SimDelta.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Time/SimClock.h"

/**
 * FSimDelta: the time step for one per-tick integration (dead reckoning, gimbal slew; ROADMAP 2.1/2.4).
 * In lockstep the sim clock moves only by Step(), so the step is the clock's advance since this instance's last
 * call (0 between steps, never negative). Otherwise the engine delta, unchanged: a frozen clock outside lockstep is
 * CIGI's "static time of day", and entities keep moving under it. One instance per consumer, so the order of
 * consumers within a frame doesn't matter.
 */
class FSimDelta
{
public:
	explicit FSimDelta(FSimClock* InClock = nullptr) : Clock(InClock) {}

	double Next(double EngineDeltaSec)
	{
		FSimClock& C = Clock ? *Clock : FSimClock::Get();
		const uint64 Now = C.NowMicros();
		const uint64 Prev = LastMicros;
		LastMicros = Now;
		if (!C.IsLockstep()) return EngineDeltaSec;
		if (Prev == 0 || Now <= Prev) return 0.0;
		return static_cast<double>(Now - Prev) * 1e-6;
	}

private:
	FSimClock* Clock = nullptr;
	uint64 LastMicros = 0;
};

namespace CamSimTime
{
	/** The sensor AE/AGC step: frame time in lockstep (a stepped clock is frozen while a capture settles), else sim time. */
	inline double SensorStepSeconds(bool bLockstep, double SimDeltaSec, float FrameRate)
	{
		if (!bLockstep) return SimDeltaSec;
		return 1.0 / static_cast<double>(FrameRate > 0.f ? FrameRate : 30.f);
	}
}
```

- [ ] **Step 4: Wire the consumers.**
  - `ACamSimEntity::Tick`: `UpdateDeadReckoning(static_cast<float>(DrDelta.Next(DeltaTime)));`
  - `ACamSimCamera::Tick`: `ApplyCigiViewState(static_cast<float>(GimbalDelta.Next(DeltaTime)));` (the `RefreshPlayerView`
    call keeps `DeltaTime`).
  - `UCamSimCaptureComponent::UpdateSensorParams`: after computing `Dt`, `const double SensorDt = CamSimTime::SensorStepSeconds(FSimClock::Get().IsLockstep(), Dt, Cfg.FrameRate);`
    and pass `SensorDt` wherever `Dt` went to the controller.
- [ ] **Step 5: Build, run `CamSim.SimDelta` and `CamSim.SensorController` → PASS.**
- [ ] **Step 6: Commit** `feat(time): sim delta for dead reckoning and gimbal slew in lockstep; AE on frame time in lockstep (2.4)`

### Task 2: `control_api` config section

**Files:** `Config/CamSimConfig.h/.cpp`, `deploy/camsim_config.yaml`, `docs/configuration.md`, Test: `Tests/ControlApiConfigTest.cpp`

**Interfaces:** Produces `FCamSimConfig::FControlApiConfig { bool bEnabled=false; float SettleLoadPct=99.9f; int32 DefaultSettleFrames=30; float CaptureTimeoutSec=120.f; } ControlApi;`

- [ ] **Step 1: Test** `CamSim.ControlApi.Config`: defaults; YAML via `LoadFromYamlString("control_api:\n  enabled: true\n  settle_load_pct: 98\n  default_settle_frames: 5\n  capture_timeout_s: 9\n")`
  sets all four; env `CAMSIM_CONTROL_API_ENABLED=1` overrides (set/unset with `FPlatformMisc::SetEnvironmentVar`, as
  `Config.EnvOverridesYaml` does); `DefaultSettleFrames` clamps to [0, 600], `SettleLoadPct` to [0, 100],
  `CaptureTimeoutSec` to [1, 3600].
- [ ] **Step 2: Run → fail.** **Step 3:** add the struct; parse in the YAML loader next to `operational`
  (`YamlBool/YamlFloat/YamlInt` on `Root["control_api"]`); env overrides `CAMSIM_CONTROL_API_ENABLED`,
  `CAMSIM_CONTROL_API_SETTLE_LOAD_PCT`, `CAMSIM_CONTROL_API_SETTLE_FRAMES`, `CAMSIM_CONTROL_API_CAPTURE_TIMEOUT_S`;
  clamps in the existing validation pass. Add the section to `deploy/camsim_config.yaml` (commented as the other
  sections) and a "Control API" section to `docs/configuration.md`.
- [ ] **Step 4: Run `CamSim.ControlApi.Config` and `CamSim.Config` → PASS** (`CanonicalConfigHasNoUnknownKeys` covers the yaml).
- [ ] **Step 5: Commit** `feat(config): control_api section (2.4)`

### Task 3: Canonical consumers for camera, view, sensor and environment

**Files:**
- Modify: `Camera/CamSimPlatformRig.h/.cpp`: public `void SetPlatformPose(const CamSimFrames::FGeoPose& Pose, double TimeSec);` (clears `bAttached`, calls `ApplyPose`).
- Modify: `Camera/CamSimGimbalComponent.h/.cpp`: `void ApplyView(const FViewCommand& View, float DeltaTime, const FCamSimConfig& Config);`; `ApplyViewControl` calls a new private `SnapAngles(bool bYawEn, float Yaw, bool bPitchEn, float Pitch, bool bRollEn, float Roll, const FCamSimConfig&)` holding today's unwind/clamp/cancel-slew body. `ApplyView` with `Snap` → `SnapAngles`; `Slew` → set slew targets and `AdvanceSlew(DeltaTime, Config)`.
- Modify: `Camera/CamSimSensorComponent.h/.cpp`: `void ApplySensor(const FSensorCommand& Cmd);` (on, waveband, polarity; logs on change; `Zoom` ignored: FOV comes from `FViewCommand`).
- Modify: `Camera/CamSimCamera.h/.cpp`: `void ApplyControlCamera(const FControlCameraCommand& Cmd);`, `void SetHostCameraSuppressed(bool b)`, `bool IsHostCameraSuppressed() const`; `static float ClampHFov(float Deg)` (1..179) used by View Definition and the API. `ApplyHostPlatformState` drains the queue and, when suppressed, `HostFrame.Reset()` and returns before applying anything.
- Create: `Control/ControlApiTypes.h`: `struct FControlCameraCommand { TOptional<CamSimFrames::FGeoPose> Pose; TOptional<FViewCommand> View; TOptional<FSensorCommand> Sensor; };`
- Modify: `Environment/CamSimEnvironment.h/.cpp`: `void ApplyAtmosphereCommand(const FAtmosphereCommand&)`, `void ApplyWeatherCommand(const FWeatherCommand&)`; pure statics `static FCigiAtmosphereState ToAtmosphereState(const FAtmosphereCommand&)`, `static FCigiWeatherState ToWeatherState(const FWeatherCommand&)` (field for field; weather `Scope` Global → 0). The Apply methods set `CurrentAtmosphere`/`CurrentWeather`, the received flags, and call the existing private `ApplyAtmosphere()`/`ApplyWeather()`.
- Test: `Tests/ControlConsumersTest.cpp`

**Interfaces:** Produces the methods above; `FControlCameraCommand` in `Control/ControlApiTypes.h`.

- [ ] **Step 1: Tests** (`CamSim.ControlApi.Consumers.*`):
  - `GimbalSnap`: `NewObject<UCamSimGimbalComponent>()`; `ApplyView` with Snap yaw 270, pitch −120 (config pitch min −110) → yaw −90, pitch −110; a following Slew command to yaw 0 with `GimbalMaxSlewRateDegPerSec` 10 and dt 1 → yaw −80.
  - `SensorApply`: `NewObject<UCamSimSensorComponent>()`; `ApplySensor({bOn=true, Waveband=IR, Polarity=1})` → mode IR, polarity 1, on.
  - `AtmosphereFields` / `WeatherFields`: every field of the command lands in the CIGI state; `Scope` Global → 0, enabled flags carried.
  - `HFovClamp`: `ClampHFov(0.2) == 1`, `ClampHFov(200) == 179`, `ClampHFov(20) == 20`.
- [ ] **Step 2: Run → fail. Step 3: implement as above. Step 4: Run `CamSim.ControlApi.Consumers` + `CamSim.Camera` + `CamSim.Phase18` → PASS.**
- [ ] **Step 5: Commit** `feat(camera,env): canonical view/sensor/pose/atmosphere/weather entry points (2.3/2.4)`

`ACamSimCamera::ApplyControlCamera`:

```cpp
void ACamSimCamera::ApplyControlCamera(const FControlCameraCommand& Cmd)
{
	if (!Subsystem) return;
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	if (Cmd.Pose) Platform.SetPlatformPose(*Cmd.Pose, FPlatformTime::Seconds());
	if (Cmd.Sensor)
	{
		SensorComp->ApplySensor(*Cmd.Sensor);
		Telemetry.SetSensor(static_cast<uint8>(SensorComp->GetMode()), SensorComp->GetPolarity());
	}
	if (Cmd.View)
	{
		if (Cmd.View->HFovDeg) SceneCapture->FOVAngle = ClampHFov(*Cmd.View->HFovDeg);
		if (Cmd.View->Gimbal != FViewCommand::EGimbal::None) GimbalComp->ApplyView(*Cmd.View, 0.f, Cfg);
		SceneCapture->SetRelativeRotation(GimbalComp->GetGimbalRelativeRotation());
		Telemetry.SetGimbal(GimbalComp->GetGimbalYaw(), GimbalComp->GetGimbalPitch(), GimbalComp->GetGimbalRoll());
	}
}
```

### Task 4: Request/response JSON (pure)

**Files:** Create `Control/ControlApiJson.h/.cpp`; Test `Tests/ControlApiJsonTest.cpp`

**Interfaces (produces):**

```cpp
namespace CamSimControl
{
	struct FParseError { FString Message; };
	template<typename T> using TParsed = TValueOrError<T, FParseError>;

	struct FClockRequest { TOptional<bool> bLockstep; TOptional<FDateTime> Utc; TOptional<double> Rate; };
	struct FStepRequest  { double Seconds = 0.0; };
	struct FEntityRequest
	{
		uint16 Id = 0;
		TOptional<uint16> TypeId;
		TOptional<CamSimFrames::FGeoPose> Pose;    // lat/lon/alt + heading/pitch/roll (CIGI), all-or-none for the position
		TOptional<ESurfaceMode> Surface;
		TOptional<bool> bHidden;
		TArray<FComponentCommand> Components;      // Key filled by the adapter
	};
	struct FEnvironmentRequest { TOptional<FAtmosphereCommand> Atmosphere; TOptional<FWeatherCommand> Clouds; TOptional<float> Beaufort; TOptional<float> SeaDirectionDeg; };
	struct FCaptureRequest { TOptional<int32> SettleFrames; TOptional<double> TimeoutSec; bool bDepth = true; };

	TParsed<FClockRequest>       ParseClock(const FString& Body);
	TParsed<FStepRequest>        ParseStep(const FString& Body);
	TParsed<FControlCameraCommand> ParseCamera(const FString& Body);
	TParsed<FEntityRequest>      ParseEntity(uint32 Id, const FString& Body);
	TParsed<FEnvironmentRequest> ParseEnvironment(const FString& Body);
	TParsed<FCaptureRequest>     ParseCapture(const FString& Body);   // empty body = defaults

	FString ErrorJson(const FString& Message);                         // {"error":"..."} escaped
	FString UtcIso(const FDateTime& Utc);                              // "2026-10-15T18:00:00.000Z"
}
```

Rules every parser applies: body must be a JSON object (an empty body is `{}` for camera/capture only); every number
field must be a JSON number and finite; unknown top-level keys → 400 `unknown field "x"` (catches typos);
camera/entity position is `lat`, `lon`, `alt` together (else 400 `lat, lon and alt go together`); `lat` in
[−90, 90], `lon` in [−180, 180]; heading/pitch/roll default 0 when a position is given; camera pose →
`FGeoPose{Lat,Lon,Alt,Neu=CamSimFrames::CigiToNeu(heading,pitch,roll)}`; `gimbal` → `FViewCommand{Gimbal=Snap, bYawEn/bPitchEn/bRollEn per present key}`;
`hfov_deg` in (0, 180); `sensor.mode` ∈ {eo, ir}, `sensor.polarity` ∈ {white_hot, black_hot}; `sensor.on` bool
(default true); entity id 1..65535; `surface` ∈ {none, ground, water}; component `class` 0..255, `id` 0..65535,
`state` 0..255; step `seconds` required, 0 < s ≤ 3600; clock `utc` via `FDateTime::ParseIso8601`, `rate` ≥ 0;
environment `atmosphere.visibility_m` > 0, `humidity_pct` 0..100; `clouds.coverage_pct` 0..100 (sets `bEnabled = coverage > 0`,
Scope Global, LayerId 1, `type` → CloudType 0..9 default 1); `sea.beaufort` 0..12; capture `settle_frames` 0..600,
`timeout_s` 1..3600.

- [ ] **Step 1: Tests** (`CamSim.ControlApi.Json.*`), one test per parser covering: a full valid body, each rule above
  failing with the expected message substring, and these named edge cases — `CameraPartialPosition` (`{"lat":1,"lon":2}` → 400),
  `CameraNaN` (`{"hfov_deg":"x"}` → 400 "number"), `UnknownField` (`{"hdg":3}` → 400 `unknown field "hdg"`),
  `EntityDefaultsHpr`, `StepZero`/`StepTooBig`, `ClockIso` (`"2026-07-01T19:30:00Z"`), `EmptyCaptureBody`,
  `ErrorJsonEscapes` (a quote and backslash round-trip through `FJsonSerializer`).
- [ ] **Step 2: Run → fail. Step 3: implement with `TJsonReaderFactory<>::Create` + `FJsonSerializer::Deserialize`;
  a small `FFieldReader` helper (tracks consumed keys for the unknown-field check; `Number(Key, Min, Max)` returns
  `TOptional<double>` or records an error). Step 4: Run → PASS.**
- [ ] **Step 5: Commit** `feat(control): JSON request parsing and validation (2.4)`

### Task 5: Capture settle gate (pure)

**Files:** Create `Control/ControlCaptureGate.h/.cpp`; Test `Tests/ControlCaptureGateTest.cpp`

**Interfaces (produces):**

```cpp
struct FControlCaptureInputs
{
	double NowSec = 0.0;
	bool   bTerrainReady = false;
	float  MinLoadProgressPct = -1.f;   // < 0: no tilesets (counts as loaded)
	bool   bVegetationIdle = true;
	bool   bCommandThisTick = false;
	uint64 FramesCaptured = 0;           // camera's capture counter
};
class FControlCaptureGate
{
public:
	enum class EState : uint8 { Idle, Waiting, Settling, Fire, TimedOut };
	void Begin(double NowSec, int32 SettleFrames, double TimeoutSec, float SettleLoadPct);
	EState Update(const FControlCaptureInputs& In);   // Fire exactly once, then Idle until Begin
	void Reset();                                     // back to Idle (request completed or abandoned)
	EState GetState() const;
	int32 FramesWaited() const;                       // captured frames since Begin
	FString PendingReason() const;                    // "terrain", "tiles 97.2%", "vegetation", "settling 12/30"
};
```

Behaviour: `Waiting` until terrain ready, load ≥ pct (or < 0) and vegetation idle and no command this tick; then
`Settling`, recording `FramesCaptured`; any failed condition or a command drops back to `Waiting`; when
`FramesCaptured - Start >= SettleFrames` → `Fire` (returned once; the caller requests the capture frame and calls
`Reset()` after delivery). `NowSec - Begin >= Timeout` in Waiting/Settling → `TimedOut` (returned once).

- [ ] **Step 1: Tests** (`CamSim.ControlApi.CaptureGate.*`): `FiresAfterSettleFrames` (30 frames after ready → Fire on the 30th, not before), `WaitsForTiles` (99.0 < 99.9 stays Waiting; −1 counts as loaded), `WaitsForVegetation`, `ResetsOnCommand` (command at frame 20 restarts the count), `ZeroSettleFiresImmediately`, `TimesOutOnce` (TimedOut returned once, then Idle), `ReasonText`.
- [ ] **Step 2–4: fail → implement → PASS. Step 5: Commit** `feat(control): capture settle gate (2.4)`

### Task 6: Capture frame plumbing (ground truth record, PNG, depth)

**Files:**
- Modify: `GroundTruth/FCocoAnnotationWriter.h/.cpp`: `static FString BuildFrameRecord(const TArray<FEntityAnnotationData>&, const FCamSimTelemetry&, uint64 FrameIdx, int32 ImageWidth, int32 ImageHeight);` (today's line body minus the trailing `\n`); `WriteFrame` writes `BuildFrameRecord(...) + "\n"`.
- Modify: `GroundTruth/FDepthMapWriter.h/.cpp`: `static TArray64<uint8> EncodeDepthPng(const TArray<float>& DepthMetres, int32 W, int32 H, float FarPlaneM);` used by `WriteDepthFrame`.
- Modify: `GroundTruth/FGroundTruthCollector.h/.cpp`: `FString WriteAnnotationFrame(TArray<FEntityAnnotationData>, const FInstanceIdImage*, const FCamSimTelemetry&, uint64 FrameIdx, bool bForce = false);` returns the record (empty when not annotated or not open); `bForce` bypasses the interval. `WriteDepthFrame(..., bool bForce=false)`. `float GetDepthFarPlaneM() const`. Fix the stale thread comments (they name `bEncoderBusy`/`FMultiViewFrameSink`; it is `bSensorBusy` in `UCamSimCaptureComponent`).
- Create: `Control/ControlCaptureJob.h`:

```cpp
struct FControlCaptureJob
{
	bool bWantDepth = true;
	// Written by the task thread before bDone; read by the game thread after.
	uint64 FrameIdx = 0;
	uint64 TimestampUs = 0;
	int32 Width = 0, Height = 0;
	FString Record;                 // the frame's camsim_coco.jsonl record ("" without ground truth)
	TArray64<uint8> Png;            // sensor output, 8-bit RGB
	TArray64<uint8> DepthPng;       // 16-bit grey, metres * 65535 / far; empty without depth
	FString Error;
	TAtomic<bool> bDone { false };
};
```

- Create: `Control/CapturePng.h/.cpp`: `TArray64<uint8> CamSimControl::EncodeNv12Png(const TArray<uint8>& Nv12, int32 W, int32 H);` (`CamSimNv12::ToBgra` + ImageWrapper PNG, as the snapshot service does).
- Modify: `Camera/CamSimCaptureComponent.h/.cpp`: `void RequestCaptureFrame(TSharedRef<FControlCaptureJob, ESPMode::ThreadSafe> Job);` (held in `PendingCaptureJob`); `Capture()` forces `bAnnotated` when a job is pending and moves it into `FSlot::CaptureJob`; `Poll()` passes it to `SubmitFrameToEncoder(..., TSharedPtr<FControlCaptureJob, ESPMode::ThreadSafe> Job)`; the background task calls the collector with `bForce = Job.IsValid()`, stores the record, copies NV12 and depth (before they move), and launches one more `AsyncTask` that encodes PNG (+ depth PNG if `bWantDepth`) and sets `bDone` (release). A failed readback for a job's frame sets `Error = "readback failed"` and `bDone`.
- Modify: `Camera/CamSimCamera.h`: forwarders `RequestCaptureFrame(...)`, `GetMinLoadProgressPct()` (stored by `FCamSimStreamingController::UpdateTerrainGate`, new `float LastMinLoadProgressPct = -1.f` + getter).
- Test: `Tests/ControlCaptureJobTest.cpp`

- [ ] **Step 1: Tests**: `CamSim.ControlApi.Capture.RecordMatchesJsonl` (golden: two entities, one masked with RLE/OBB/box3d/geo and one projected; `BuildFrameRecord` equals the line `WriteFrame` writes minus `\n`, and parses as JSON with `frame_id`, `annotations[1].mask_source == "projection"`); `DepthPngRoundTrip` (2×2 depth [0, 2500, 5000, 9999] m, far 5000 → decoded 16-bit values 0, 32768 (±1), 65535, 65535); `Nv12PngSize` (a 4×2 grey NV12 → PNG decodes to 4×2).
- [ ] **Step 2–4: fail → implement → PASS; also run `CamSim.GroundTruth` (unchanged output).**
- [ ] **Step 5: Commit** `feat(capture): tag one capture frame for the control API; record/PNG/depth reuse the writers (2.4)`

### Task 7: Host adapter for API entities

**Files:** Create `Control/ControlApiAdapter.h/.cpp`; Modify `Entity/CamSimEntityManager.h/.cpp` (poll after DIS, before `ApplyHostPlatformState`); Modify `Subsystem/CamSimSubsystem.h/.cpp` (own the service from Task 8; `GetControlApiAdapter()`); Test `Tests/ControlApiAdapterTest.cpp`

**Interfaces (produces):**

```cpp
class FControlApiAdapter
{
public:
	/** Apply a parsed entity request; returns false + error for a new entity without type or position. */
	bool PutEntity(const CamSimControl::FEntityRequest& Req, FString& OutError);
	bool RemoveEntity(uint16 Id);          // false: unknown
	void RemoveAll();
	/** Every API entity's last state, re-submitted each tick (D9); removals once; components once. */
	void Poll(ISimCommandSink& Sink);
	TArray<uint16> GetIds() const;
	bool WasCommandedThisFrame(uint64 FrameCounter) const;   // camera or entity command applied this frame
	void NoteCommand(uint64 FrameCounter);
	static FEntityKey KeyFor(uint16 Id) { return FEntityKey(EHostSource::ControlApi, Id); }
};
```

- [ ] **Step 1: Tests** with a recording `ISimCommandSink`: `NewEntityNeedsTypeAndPose`; `ResubmitsEveryPoll` (two polls → two `FEntityCommand`s, same pose, key `api:7`); `PartialUpdateKeepsPose` (put heading only → previous lat/lon kept); `RemoveSubmitsOnce` (Remove lifecycle once, then nothing); `ComponentsOnce`; `HiddenLifecycle`; `SurfaceMode` (ground → `ESurfaceMode::Ground`).
- [ ] **Step 2–4: fail → implement → PASS. Step 5: Commit** `feat(control): API entity host adapter (2.4)`

### Task 8: Routes, service, `/ready`

**Files:**
- Modify: `Health/CamSimHealthServer.h/.cpp`: `void BindRoute(const FString& Path, EHttpServerRequestVerbs Verbs, FHttpRequestHandler Handler);` kept in a list and (re)bound in `BindRoutes` like the snapshot handlers (which become calls to it). `FHttpRequestHandler` receives `FHttpServerRequest` (path params via `Req.PathParams`, body `Req.Body`).
- Create: `Control/ControlApiService.h/.cpp`: owns `FControlApiAdapter`, `FControlCaptureGate`, the pending capture `{FHttpResultCallback, TSharedPtr<FControlCaptureJob>}`; `void Bind(FCamSimHealthServer&)`; `void Tick()` (game thread, from `UCamSimSubsystem::Tick`: gate update, fire, deliver/timeout, 503 on shutdown); `bool OwnsCamera() const`. Routes as spec §3; status JSON as §3.1; capture response as §3.6 (base64 via `FBase64::Encode`; `annotation` is the raw record spliced in, `null` when empty).
- Modify: `Subsystem/CamSimSubsystem.cpp`: create the service when `Config.ControlApi.bEnabled && HealthServer`; log a warning if enabled without the health server; `IsCigiReady` lambda → `CigiPackets > 0 || (ControlApi && ControlApi->OwnsCamera())`; `bool IsVegetationIdle() const`; `Tick` calls `ControlApi->Tick()`.
- Test: `Tests/ControlApiRoutesTest.cpp` (two-ticker pattern from `HttpServerLifecycleTest.cpp`, port 48081, no world: `UCamSimSubsystem` absent, so the service is constructed with null hooks)

`FControlApiService` takes its world hooks as a struct so the route tests can fake them:

```cpp
struct FControlApiHooks
{
	TFunction<ACamSimCamera*()> Camera;                  // null in tests
	TFunction<ACamSimEnvironment*()> Environment;
	TFunction<void(float Beaufort, float DirDeg)> ApplySea;
	TFunction<bool()> VegetationIdle;
	TFunction<bool(const FEntityKey&, CamSimFrames::FGeoPose&)> EntityPose;
	TFunction<FString()> GroundTruthJson;                 // {"masks":..,"depth":..}
	FSimClock* Clock = nullptr;                           // null = FSimClock::Get()
};
```

- [ ] **Step 1: Tests** (`CamSim.ControlApi.Routes.*`): `StatusShape` (200, keys present); `ClockLockstepAndStep` (PUT lockstep, step 10 → utc +10 s; step without lockstep → 409; rate in lockstep → 409); `EntityCrud` (PUT 2001 with pose → 200; GET lists id; DELETE → 200; DELETE again → 404; bad id `/entities/0` → 400); `CameraWithoutCamera503`; `CaptureWithoutCamera503`; `SecondCapture409` (inject a pending capture through a test hook, a second POST → 409); `BadJson400`; `ReadyCountsApiOwnership` (health server `/ready` with IsCigiReady from the service's `OwnsCamera`).
- [ ] **Step 2–4: fail → implement → PASS. Run `CamSim.HttpServer` and `CamSim.SnapshotEndpoint` too (BindRoute refactor).**
- [ ] **Step 5: Commit** `feat(control): /api/v1 routes on the health server; /ready without CIGI once the API owns the camera (2.4)`

### Task 9: Full headless suite + build

- [ ] Run `scripts/run.sh --build-only`, then the full NullRHI suite (`RunTests CamSim`). Expected: all pass (previous
  count + new tests). Fix any regression before going on. Commit fixes with their own messages.

### Task 10: Python client, COCO writer, randomizer, generator

**Files:** Create `scripts/control/pyproject.toml` (`camsim-control`, Python ≥ 3.11, no dependencies; dev: pytest),
`scripts/control/camsim_control/{__init__,client,coco,randomize}.py`, `scripts/control/make_dataset.py`,
`scripts/control/tests/{test_client,test_coco,test_randomize}.py`, `scripts/control/README.md`.

Interfaces:

```python
@dataclass(frozen=True)
class Capture:
    frame_id: int; utc: str; timestamp_us: int; width: int; height: int; sensor_mode: str
    png: bytes; depth_png: bytes | None; depth_far_plane_m: float; record: dict | None; settle: dict

class ApiError(RuntimeError):  # .status, .message
    ...

class Client:
    def __init__(self, base: str = "http://127.0.0.1:8080", timeout: float = 30.0): ...
    def status(self) -> dict
    def set_clock(self, *, lockstep: bool | None = None, utc: str | None = None, rate: float | None = None) -> dict
    def step(self, seconds: float) -> dict
    def set_camera(self, **fields) -> dict            # lat/lon/alt/heading/pitch/roll/gimbal/hfov_deg/sensor
    def get_camera(self) -> dict
    def release_camera(self) -> None
    def put_entity(self, entity_id: int, **fields) -> dict
    def delete_entity(self, entity_id: int) -> None
    def clear_entities(self) -> None
    def entities(self) -> list[dict]
    def set_environment(self, **fields) -> dict
    def capture(self, *, settle_frames: int | None = None, timeout_s: float | None = None, depth: bool = True) -> Capture
    def wait_ready(self, timeout_s: float = 900.0) -> None   # polls /api/v1/status

class DatasetWriter:   # coco.py
    def __init__(self, out_dir: Path, categories: dict[int, str]): ...
    def add(self, name: str, cap: Capture, extra: dict | None = None) -> int   # writes images/<name>.png, depth/<name>.png
    def close(self) -> Path                                                     # annotations.json (COCO)

@dataclass(frozen=True)
class Sample:  # randomize.py
    index: int; camera: dict; utc: str; environment: dict; entities: list[dict]
def sample(seed: int, index: int, site: tuple[float, float], ground_alt: float, types: list[int]) -> Sample
```

COCO conversion: `images[{id, file_name, width, height, camsim:{frame_id, timestamp_us, utc, sensor_mode, platform}}]`,
`annotations[{id, image_id, category_id = category.id, bbox, area, iscrowd 0, segmentation (RLE as given, when present),
camsim:{entity_id, source, source_id, mask_source, visibility, bbox_amodal, obb, obb_amodal, truncation, box3d, geo}}]`,
`categories[{id, name}]` from the type → class-name map (`--types` with names from `deploy/camsim_config.yaml`'s
`entity_types`, read with a tiny YAML-free scan or passed explicitly).

Randomizer: `rng = random.Random(f"{seed}:{index}")`; camera slant range 300–2000 m, depression 20–90°, heading
0–360, hfov 5–40°; aim point within 50 m of the site; entities 1–4, types drawn from `types`, offsets within
0.4 × the footprint half-width, heading 0–360, `surface` ground (boats water); UTC hour 6–18 local solar at a fixed
date; visibility 5–50 km; cloud coverage 0–60 %; Beaufort 0–4. Camera position computed from aim point, heading,
depression and range on a local tangent plane (`scripts/bench/scenario.py`'s flat-earth helper or an equivalent
`offset_ll(lat, lon, north_m, east_m)`).

`make_dataset.py --out DIR --count N --seed S --site LAT,LON --ground-alt M [--modes eo,ir] [--types 2001,3001] [--base URL]`:
`wait_ready`, `set_clock(lockstep=True)`, then per sample: clear entities, put entities, set environment, set clock
utc, set camera (pose + gimbal + fov + sensor per mode), capture per mode, `writer.add`. Prints a one-line summary
per sample and the dataset path.

- [ ] **Step 1: Tests**: client against a `http.server` thread (routes echo; error body → `ApiError` with status and
  message; capture decodes base64); COCO from a recorded capture record (masked + projected annotation; image ids;
  category ids; RLE preserved; `json.loads` round trip); randomizer determinism (`sample(7, 3)` equal across calls and
  independent of sampling index 2 first; values in range).
- [ ] **Step 2: `uv run --project scripts/control --with pytest pytest scripts/control/tests` → fail. Step 3: implement. Step 4: → PASS.**
- [ ] **Step 5: Commit** `feat(control): Python client, COCO dataset writer, seeded randomizer, make_dataset.py (2.4)`

### Task 11: Acceptance `scripts/control_api_check.py`

**Files:** Create `scripts/control_api_check.py`.

- Launch: `camsim_session.config_override` not needed; env `CAMSIM_CONTROL_API_ENABLED=1`, `CAMSIM_ML_ENABLED=1`,
  `CAMSIM_ML_INTERVAL_FRAMES=100000`, `CAMSIM_ML_OUTPUT_DIR=<out>/ml`, `CAMSIM_SCENE_DIR=<pkg>` (when `--scene`), started
  with `scripts/run.sh --headless --local --detach` and **no CIGI host**; wait with `Client.wait_ready`; teardown
  `scripts/stop.sh`. (Reuse `rb.wait_port_free`.)
- Gates C1–C8 exactly as spec §6. C4 uses a CIGI HOT request (`send_cigi_test.pack_hat_hot_request` +
  `parse_query_responses`, IG Control first, responses on the host port) at the truck's lat/lon. C5 starts `rb.Host`
  with a pose 5 km away for 3 s while the API owns the camera, compares `get_camera()` before/after, releases, and
  checks the KLV-reported position moves (via `GET /api/v1/camera`). C6 runs `make_dataset.py` in-process (import) and
  loads with `pycocotools` (`uv run --with pycocotools --with numpy`). C7: two launches.
- Output: `<out>/report.md` with each gate PASS/FAIL and numbers, `<out>/dataset/`, overlays for C3
  (`gt_check_lib.draw_overlay`).
- Site: the georeg flat site (33.3072917, −117.344625) with `--scene .cache/scene-packages/pendleton-bridges`.

- [ ] Run it on the Linux box; fix what fails (each fix with a test first where it's C++ logic); commit
  `test(control): acceptance check C1–C8 (2.4)` and the fixes.

### Task 12: Docs and roadmap

- `docs/control-api.md` (new guide: enable, routes with examples, lockstep semantics, capture semantics and
  settle conditions, Python client and `make_dataset.py`, Docker use, limits).
- `docs/configuration.md` (Task 2), `docs/testing.md` (map: control API changes → `CamSim.ControlApi`, the pytest
  command, `control_api_check.py --runs`).
- `ROADMAP.md`: 2.4 status with the measured results; 2.1 "Still open" (lockstep driver done; DR + slew on sim delta
  in lockstep); R6 open item on AE under a frozen clock answered for lockstep.
- `CLAUDE.md`: commands table (client tests, `make_dataset.py`, `control_api_check.py`), architecture (`Control/`),
  test counts (recount with the documented grep), a gotcha line for the control API.
- [ ] Commit `docs: control API guide, ROADMAP 2.4 done, test counts`.
