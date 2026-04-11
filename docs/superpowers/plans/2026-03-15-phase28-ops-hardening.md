# Phase 28 — Operational Hardening (Sprint 1) Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make CamSim production-ready for 24/7 Docker/K8s deployment with config validation, structured logging, HTTP health probes, per-frame latency tracking, and CI test execution.

**Architecture:** Five independent features layered onto the existing UE5 subsystem. Config validation (28D) is pure logic on `FCamSimConfig`. Structured logging (28B) and latency tracking (28G) are new structs owned by `FSubsystemImpl`. HTTP health (28C) uses UE5's `HTTPServer` module. CI tests (28A) is a workflow-only change.

**Tech Stack:** UE5.7 C++ (no UObject), `FHttpServerModule`/`IHttpRouter`, `TSpscQueue`, `IFileHandle`, `FPlatformTime::Cycles64()`, GitHub Actions

**Spec:** `docs/superpowers/specs/2026-03-15-phase28-ops-hardening-design.md`

---

## File Map

| File | Responsibility | Action |
|------|----------------|--------|
| `Config/CamSimConfig.h` | Add `FOperationalConfig` struct, `Validate()` declaration, `bTrackPipelineLatency` field | Modify |
| `Config/CamSimConfig.cpp` | Implement `Validate()`, parse `operational:` YAML block + env vars | Modify |
| `Tests/Phase28OpsTest.cpp` | All Phase 28 unit tests (validation, latency tracker, logger) | Create |
| `Logging/CamSimJsonLogger.h` | `FCamSimJsonLogger` header — SPSC queue, Open/Close/Log/Flush API | Create |
| `Logging/CamSimJsonLogger.cpp` | Implementation — JSONL file I/O, rotation, ISO 8601 timestamps | Create |
| `Diagnostics/PipelineLatencyTracker.h` | `FPipelineLatencyTracker` header — Mark*, CommitFrame, ComputePercentiles | Create |
| `Diagnostics/PipelineLatencyTracker.cpp` | Implementation — ring buffer, atomic write index, percentile sort | Create |
| `Health/CamSimHealthServer.h` | `FCamSimHealthServer` header — Start/Stop/UpdateTick | Create |
| `Health/CamSimHealthServer.cpp` | Implementation — FHttpServerModule routes for /live, /ready, /metrics | Create |
| `CamSimTest.Build.cs` | Add `"HTTPServer"` module dependency | Modify |
| `Subsystem/CamSimSubsystem.cpp` | Own new components, call Validate(), emit structured logs, write latency percentiles | Modify |
| `Camera/CamSimCamera.h` | Add `FPipelineLatencyTracker*` member | Modify |
| `Camera/CamSimCamera.cpp` | Record latency timestamps at pipeline stages | Modify |
| `Encoder/EncoderThread.h` | Add `FPipelineLatencyTracker*` member | Modify |
| `Encoder/EncoderThread.cpp` | Record encode-complete + CommitFrame | Modify |
| `deploy/camsim_config.yaml` | Add `operational:` YAML block | Modify |
| `.github/workflows/ci.yml` | Add `unit-tests` job | Modify |

All source paths are relative to `unreal_project/CamSimTest/Source/CamSimTest/`.

---

## Chunk 1: Config Validation (28D)

### Task 1: Write config validation tests

**Files:**
- Create: `Tests/Phase28OpsTest.cpp`

- [ ] **Step 1: Create test file with 5 config validation tests**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// Phase 28D: Config Validation Tests
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28ValidConfigTest,
	"CamSim.Phase28.Config.ValidConfig_ReturnsNoErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28ValidConfigTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	TArray<FString> Errors = Cfg.Validate();
	TestEqual(TEXT("Default config has no validation errors"), Errors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28InvalidPortTest,
	"CamSim.Phase28.Config.InvalidPort_ReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28InvalidPortTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.MulticastPort = 99999;
	TArray<FString> Errors = Cfg.Validate();
	TestTrue(TEXT("Errors list is non-empty"), Errors.Num() > 0);
	bool bFoundPortError = false;
	for (const FString& E : Errors)
	{
		if (E.Contains(TEXT("MulticastPort")))
		{
			bFoundPortError = true;
			break;
		}
	}
	TestTrue(TEXT("Error mentions MulticastPort"), bFoundPortError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28InvalidResolutionTest,
	"CamSim.Phase28.Config.InvalidResolution_ReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28InvalidResolutionTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.CaptureWidth = 0;
	TArray<FString> Errors = Cfg.Validate();
	TestTrue(TEXT("Has errors for width=0"), Errors.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28GimbalLimitsTest,
	"CamSim.Phase28.Config.GimbalLimitsInverted_ReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28GimbalLimitsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.GimbalPitchMin = 30.0f;
	Cfg.GimbalPitchMax = -90.0f; // inverted
	TArray<FString> Errors = Cfg.Validate();
	bool bFoundGimbal = false;
	for (const FString& E : Errors)
	{
		if (E.Contains(TEXT("GimbalPitch")))
		{
			bFoundGimbal = true;
			break;
		}
	}
	TestTrue(TEXT("Error mentions GimbalPitch"), bFoundGimbal);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28OddWidthTest,
	"CamSim.Phase28.Config.OddWidth_ReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28OddWidthTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.CaptureWidth = 1921; // odd
	TArray<FString> Errors = Cfg.Validate();
	bool bFoundEven = false;
	for (const FString& E : Errors)
	{
		if (E.Contains(TEXT("even")))
		{
			bFoundEven = true;
			break;
		}
	}
	TestTrue(TEXT("Error mentions even requirement"), bFoundEven);
	return true;
}
```

- [ ] **Step 2: Verify tests fail (Validate() doesn't exist yet)**

The project won't compile until `Validate()` is declared. This is expected — TDD red phase.

### Task 2: Implement config validation

**Files:**
- Modify: `Config/CamSimConfig.h` — add `Validate()` declaration
- Modify: `Config/CamSimConfig.cpp` — implement `Validate()`

- [ ] **Step 3: Declare Validate() in CamSimConfig.h**

Add after the `static FString GetConfigFilePath();` line (around line 689):

```cpp
	/** Pre-flight validation — returns empty array if config is valid. */
	TArray<FString> Validate() const;
```

- [ ] **Step 4: Implement Validate() in CamSimConfig.cpp**

Add at the end of the file, before any closing namespace:

```cpp
// ---------------------------------------------------------------------------
// Phase 28D: Config Validation
// ---------------------------------------------------------------------------

TArray<FString> FCamSimConfig::Validate() const
{
	TArray<FString> Errors;

	auto RangeCheckInt = [&](const TCHAR* Name, int32 Value, int32 Min, int32 Max)
	{
		if (Value < Min || Value > Max)
		{
			Errors.Add(FString::Printf(TEXT("%s=%d out of range [%d, %d]"), Name, Value, Min, Max));
		}
	};

	auto RangeCheckFloat = [&](const TCHAR* Name, float Value, float Min, float Max)
	{
		if (Value < Min || Value > Max)
		{
			Errors.Add(FString::Printf(TEXT("%s=%.2f out of range [%.1f, %.1f]"), Name, Value, Min, Max));
		}
	};

	// Resolution — must be in range AND even (H.264 requires even dimensions)
	RangeCheckInt(TEXT("CaptureWidth"), CaptureWidth, 64, 7680);
	RangeCheckInt(TEXT("CaptureHeight"), CaptureHeight, 64, 4320);
	if (CaptureWidth % 2 != 0)
	{
		Errors.Add(FString::Printf(TEXT("CaptureWidth=%d must be even (H.264 requirement)"), CaptureWidth));
	}
	if (CaptureHeight % 2 != 0)
	{
		Errors.Add(FString::Printf(TEXT("CaptureHeight=%d must be even (H.264 requirement)"), CaptureHeight));
	}

	// Video
	RangeCheckInt(TEXT("VideoBitrate"), VideoBitrate, 100000, 100000000);
	RangeCheckFloat(TEXT("FrameRate"), FrameRate, 1.0f, 120.0f);

	// Ports
	RangeCheckInt(TEXT("CigiPort"), CigiPort, 1, 65535);
	RangeCheckInt(TEXT("CigiResponsePort"), CigiResponsePort, 1, 65535);
	RangeCheckInt(TEXT("MulticastPort"), MulticastPort, 1, 65535);

	// FOV
	if (HFovDeg <= 0.0f || HFovDeg > 180.0f)
	{
		Errors.Add(FString::Printf(TEXT("HFovDeg=%.2f out of range (0, 180]"), HFovDeg));
	}

	// Gimbal limits
	if (GimbalPitchMin >= GimbalPitchMax)
	{
		Errors.Add(FString::Printf(TEXT("GimbalPitchMin (%.1f) >= GimbalPitchMax (%.1f)"),
			GimbalPitchMin, GimbalPitchMax));
	}
	if (GimbalYawMin >= GimbalYawMax)
	{
		Errors.Add(FString::Printf(TEXT("GimbalYawMin (%.1f) >= GimbalYawMax (%.1f)"),
			GimbalYawMin, GimbalYawMax));
	}

	// Entities
	RangeCheckInt(TEXT("MaxEntities"), MaxEntities, 1, 10000);

	// Time
	RangeCheckFloat(TEXT("StartHour"), StartHour, 0.0f, 24.0f);

	// Watchdog
	RangeCheckInt(TEXT("WatchdogMaxReconnects"), WatchdogMaxReconnects, 0, 100);
	RangeCheckInt(TEXT("EncoderWatchdogIntervalTicks"), EncoderWatchdogIntervalTicks, 30, 9000);

	// Readback
	RangeCheckInt(TEXT("ReadbackReadyPolls"), ReadbackReadyPolls, 0, 10);

	// Codec enum
	{
		const FString Lower = VideoCodec.ToLower();
		if (Lower != TEXT("h264") && Lower != TEXT("h265"))
		{
			Errors.Add(FString::Printf(TEXT("VideoCodec='%s' must be h264 or h265"), *VideoCodec));
		}
	}

	// Encoder enum
	{
		const FString Lower = Encoder.ToLower();
		if (Lower != TEXT("auto") && Lower != TEXT("nvenc") &&
		    Lower != TEXT("libx264") && Lower != TEXT("libx265"))
		{
			Errors.Add(FString::Printf(TEXT("Encoder='%s' must be auto, nvenc, libx264, or libx265"), *Encoder));
		}
	}

	// Performance
	RangeCheckFloat(TEXT("Performance.RenderFrameRateHz"), Performance.RenderFrameRateHz, 1.0f, 120.0f);
	RangeCheckFloat(TEXT("Performance.OutputFrameRateHz"), Performance.OutputFrameRateHz, 1.0f, 120.0f);
	if (Performance.OutputFrameRateHz > Performance.RenderFrameRateHz)
	{
		Errors.Add(FString::Printf(
			TEXT("Performance.OutputFrameRateHz (%.1f) exceeds RenderFrameRateHz (%.1f)"),
			Performance.OutputFrameRateHz, Performance.RenderFrameRateHz));
	}

	return Errors;
}
```

- [ ] **Step 5: Build and run validation tests**

All 5 tests should pass. The default config has no errors; each mutated config produces the expected error.

- [ ] **Step 6: Wire Validate() into UCamSimSubsystem::Initialize()**

In `Subsystem/CamSimSubsystem.cpp`, after `Config = FCamSimConfig::Load();` (line ~255), add:

```cpp
	// Phase 28D: pre-flight config validation
	{
		const TArray<FString> ValidationErrors = Config.Validate();
		for (const FString& Err : ValidationErrors)
		{
			UE_LOG(LogCamSim, Error, TEXT("Config validation: %s"), *Err);
		}
		if (ValidationErrors.Num() > 0)
		{
			UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: %d config validation error(s) — check config"),
				ValidationErrors.Num());
			Config.bLoadedSuccessfully = false;
		}
	}
```

- [ ] **Step 7: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase28OpsTest.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp
git commit -m "feat(28D): add config pre-flight validation with 19 range checks"
```

---

## Chunk 2: Structured JSON Logging (28B)

### Task 3: Add FOperationalConfig to CamSimConfig

**Files:**
- Modify: `Config/CamSimConfig.h`
- Modify: `Config/CamSimConfig.cpp`
- Modify: `deploy/camsim_config.yaml`

- [ ] **Step 8: Add FOperationalConfig struct to CamSimConfig.h**

Add before `bool bLoadedSuccessfully` (around line 682):

```cpp
	// Phase 28 — Operational Hardening
	struct FOperationalConfig
	{
		// 28B: Structured JSON logging sidecar
		FString StructuredLogPath;           // empty = disabled
		int32   StructuredLogMaxMB = 100;    // rotation threshold

		// 28C: HTTP health endpoints
		bool  bHealthHttpEnabled = false;
		int32 HealthHttpPort     = 8080;
	};
	FOperationalConfig Operational;
```

- [ ] **Step 9: Add `bTrackPipelineLatency` to FPerformanceConfig**

In `FPerformanceConfig` (around line 480), add:

```cpp
		// 28G Per-Frame Latency Tracking
		bool  bTrackPipelineLatency = false;
```

- [ ] **Step 10: Parse operational YAML block + env vars in CamSimConfig.cpp**

In the `Load()` function, add after the last YAML parsing block (after the performance block parsing), a new block:

```cpp
	// Phase 28: operational config
	if (Root.has_child("operational"))
	{
		ryml::ConstNodeRef OpNode = Root["operational"];
		YamlString(OpNode, "structured_log_path", Cfg.Operational.StructuredLogPath);
		YamlInt(OpNode, "structured_log_max_mb", Cfg.Operational.StructuredLogMaxMB);
		YamlBool(OpNode, "health_http_enabled", Cfg.Operational.bHealthHttpEnabled);
		YamlInt(OpNode, "health_http_port", Cfg.Operational.HealthHttpPort);
	}
```

In `ApplyEnvOverrides()`, add:

```cpp
	Cfg.Operational.StructuredLogPath = GetEnv(TEXT("CAMSIM_STRUCTURED_LOG_PATH"), Cfg.Operational.StructuredLogPath);
	Cfg.Operational.StructuredLogMaxMB = GetEnvInt(TEXT("CAMSIM_STRUCTURED_LOG_MAX_MB"), Cfg.Operational.StructuredLogMaxMB);
	Cfg.Operational.bHealthHttpEnabled = GetEnvBool(TEXT("CAMSIM_HEALTH_HTTP_ENABLED"), Cfg.Operational.bHealthHttpEnabled);
	Cfg.Operational.HealthHttpPort = GetEnvInt(TEXT("CAMSIM_HEALTH_HTTP_PORT"), Cfg.Operational.HealthHttpPort);
	Cfg.Performance.bTrackPipelineLatency = GetEnvBool(TEXT("CAMSIM_TRACK_PIPELINE_LATENCY"), Cfg.Performance.bTrackPipelineLatency);
```

Also parse `track_pipeline_latency` in the performance YAML block:

```cpp
	YamlBool(PerfNode, "track_pipeline_latency", Cfg.Performance.bTrackPipelineLatency);
```

- [ ] **Step 11: Add operational block to deploy/camsim_config.yaml**

Append before the `entity_types:` section (around line 870):

```yaml
# --- Phase 28: Operational Hardening ---

# Structured JSON logging sidecar for ELK/Datadog/Splunk ingestion.
# Set to a file path to enable (e.g., /var/log/camsim.jsonl).
# Env: CAMSIM_STRUCTURED_LOG_PATH
operational:
  structured_log_path: ""

  # Max file size in MB before rotation (close, rename .1, reopen).
  # Env: CAMSIM_STRUCTURED_LOG_MAX_MB
  structured_log_max_mb: 100

  # HTTP health endpoints for Kubernetes liveness/readiness probes.
  # GET /live — 200 if game loop ticking, 503 if stalled
  # GET /ready — 200 when encoder + CIGI + first frame all OK
  # GET /metrics — Prometheus exposition format
  # Env: CAMSIM_HEALTH_HTTP_ENABLED / CAMSIM_HEALTH_HTTP_PORT
  health_http_enabled: false
  health_http_port: 8080
```

- [ ] **Step 12: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h \
        unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
        deploy/camsim_config.yaml
git commit -m "feat(28B): add FOperationalConfig struct and YAML/env parsing"
```

### Task 4: Write structured logger tests

**Files:**
- Modify: `Tests/Phase28OpsTest.cpp`

- [ ] **Step 13: Add logger tests to Phase28OpsTest.cpp**

Append to the test file:

```cpp
// -------------------------------------------------------------------------
// Phase 28B: Structured JSON Logger Tests
// -------------------------------------------------------------------------

#include "Logging/CamSimJsonLogger.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28LoggerWritesJsonTest,
	"CamSim.Phase28.Logger.WritesValidJsonLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28LoggerWritesJsonTest::RunTest(const FString& Parameters)
{
	const FString TestPath = FPaths::Combine(
		FPlatformProcess::UserTempDir(), TEXT("camsim_test_log.jsonl"));

	FCamSimJsonLogger Logger;
	TestTrue(TEXT("Logger opens"), Logger.Open(TestPath));

	TMap<FString, FString> Fields;
	Fields.Add(TEXT("codec"), TEXT("h264"));
	Logger.Log(TEXT("info"), TEXT("encoder"), TEXT("opened"), Fields);
	Logger.Flush();
	Logger.Close();

	FString Contents;
	TestTrue(TEXT("File exists"), FFileHelper::LoadFileToString(Contents, *TestPath));
	TestTrue(TEXT("Contains severity"), Contents.Contains(TEXT("\"severity\":\"info\"")));
	TestTrue(TEXT("Contains category"), Contents.Contains(TEXT("\"category\":\"encoder\"")));
	TestTrue(TEXT("Contains msg"), Contents.Contains(TEXT("\"msg\":\"opened\"")));
	TestTrue(TEXT("Contains codec field"), Contents.Contains(TEXT("\"codec\":\"h264\"")));
	TestTrue(TEXT("Contains ts"), Contents.Contains(TEXT("\"ts\":")));

	// Clean up
	IFileManager::Get().Delete(*TestPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28LoggerDisabledTest,
	"CamSim.Phase28.Logger.DisabledLogger_NoFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28LoggerDisabledTest::RunTest(const FString& Parameters)
{
	FCamSimJsonLogger Logger;
	// Don't call Open — logger should be a no-op
	Logger.Log(TEXT("info"), TEXT("test"), TEXT("should not crash"));
	TestFalse(TEXT("Logger is not open"), Logger.IsOpen());
	return true;
}
```

- [ ] **Step 14: Verify tests fail (CamSimJsonLogger doesn't exist yet)**

### Task 5: Implement FCamSimJsonLogger

**Files:**
- Create: `Logging/CamSimJsonLogger.h`
- Create: `Logging/CamSimJsonLogger.cpp`

- [ ] **Step 15: Create CamSimJsonLogger.h**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/SpscQueue.h"

/**
 * FCamSimJsonLogger
 *
 * Writes structured JSON lines to a sidecar file for ELK/Datadog ingestion.
 * Thread-safe via lock-free SPSC queue: producers enqueue entries, game-thread
 * Flush() writes them to disk. Call Flush() periodically from the subsystem tick.
 *
 * Log rotation: when the file exceeds StructuredLogMaxMB, it is closed,
 * renamed to .1, and a fresh file is opened.
 */
struct FCamSimJsonLogger
{
	bool Open(const FString& FilePath, int32 MaxSizeMB = 100);
	void Close();
	bool IsOpen() const { return FileHandle != nullptr; }

	/** Enqueue a structured log entry. Safe to call from any thread. */
	void Log(const TCHAR* Severity, const TCHAR* Category,
	         const TCHAR* Message, const TMap<FString, FString>& Fields = {});

	/** Flush queued entries to disk. Call from game thread (e.g., subsystem tick). */
	void Flush();

private:
	struct FLogEntry
	{
		FString Severity;
		FString Category;
		FString Message;
		TMap<FString, FString> Fields;
		FDateTime Timestamp;
	};

	IFileHandle* FileHandle = nullptr;
	FString FilePath;
	int64 MaxSizeBytes = 100 * 1024 * 1024;
	int64 CurrentSizeBytes = 0;
	TSpscQueue<FLogEntry> Queue;

	void WriteEntry(const FLogEntry& Entry);
	void RotateIfNeeded();
	FString FormatEntry(const FLogEntry& Entry) const;
};
```

- [ ] **Step 16: Create CamSimJsonLogger.cpp**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Logging/CamSimJsonLogger.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"

bool FCamSimJsonLogger::Open(const FString& InPath, int32 MaxSizeMB)
{
	Close();
	FilePath = InPath;
	MaxSizeBytes = static_cast<int64>(MaxSizeMB) * 1024 * 1024;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FileHandle = PlatformFile.OpenWrite(*FilePath, /*bAppend=*/true, /*bAllowRead=*/true);
	if (FileHandle)
	{
		CurrentSizeBytes = PlatformFile.FileSize(*FilePath);
	}
	return FileHandle != nullptr;
}

void FCamSimJsonLogger::Close()
{
	Flush();
	if (FileHandle)
	{
		delete FileHandle;
		FileHandle = nullptr;
	}
	CurrentSizeBytes = 0;
}

void FCamSimJsonLogger::Log(const TCHAR* Severity, const TCHAR* Category,
                            const TCHAR* Message, const TMap<FString, FString>& Fields)
{
	if (!FileHandle) return;

	FLogEntry Entry;
	Entry.Severity = Severity;
	Entry.Category = Category;
	Entry.Message = Message;
	Entry.Fields = Fields;
	Entry.Timestamp = FDateTime::UtcNow();
	Queue.Enqueue(MoveTemp(Entry));
}

void FCamSimJsonLogger::Flush()
{
	if (!FileHandle) return;

	FLogEntry Entry;
	while (Queue.Dequeue(Entry))
	{
		RotateIfNeeded();
		WriteEntry(Entry);
	}
}

FString FCamSimJsonLogger::FormatEntry(const FLogEntry& Entry) const
{
	// Build JSON manually — avoids FJsonObject allocation overhead for simple flat objects
	FString Json = TEXT("{");
	Json += FString::Printf(TEXT("\"ts\":\"%s\""), *Entry.Timestamp.ToIso8601());
	Json += FString::Printf(TEXT(",\"severity\":\"%s\""), *Entry.Severity);
	Json += FString::Printf(TEXT(",\"category\":\"%s\""), *Entry.Category);

	// Escape quotes in message
	FString EscapedMsg = Entry.Message;
	EscapedMsg.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedMsg.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Json += FString::Printf(TEXT(",\"msg\":\"%s\""), *EscapedMsg);

	for (const auto& Pair : Entry.Fields)
	{
		FString EscapedVal = Pair.Value;
		EscapedVal.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedVal.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Json += FString::Printf(TEXT(",\"%s\":\"%s\""), *Pair.Key, *EscapedVal);
	}
	Json += TEXT("}\n");
	return Json;
}

void FCamSimJsonLogger::WriteEntry(const FLogEntry& Entry)
{
	if (!FileHandle) return;
	const FString Line = FormatEntry(Entry);
	auto Utf8 = StringCast<UTF8CHAR>(*Line);
	const int32 Len = Utf8.Length();
	FileHandle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Len);
	CurrentSizeBytes += Len;
}

void FCamSimJsonLogger::RotateIfNeeded()
{
	if (CurrentSizeBytes < MaxSizeBytes) return;

	delete FileHandle;
	FileHandle = nullptr;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString RotatedPath = FilePath + TEXT(".1");
	PlatformFile.DeleteFile(*RotatedPath);
	PlatformFile.MoveFile(*RotatedPath, *FilePath);

	FileHandle = PlatformFile.OpenWrite(*FilePath, /*bAppend=*/false, /*bAllowRead=*/true);
	CurrentSizeBytes = 0;
}
```

- [ ] **Step 17: Build and run logger tests**

Both `WritesValidJsonLine` and `DisabledLogger_NoFile` should pass.

- [ ] **Step 18: Wire logger into UCamSimSubsystem**

In `Subsystem/CamSimSubsystem.cpp`:

1. Add include: `#include "Logging/CamSimJsonLogger.h"`

2. Add to `FSubsystemImpl` struct:
```cpp
	TUniquePtr<FCamSimJsonLogger> JsonLogger;
```

3. In `Initialize()`, after config validation block, add:
```cpp
	// Phase 28B: structured JSON logging
	if (!Config.Operational.StructuredLogPath.IsEmpty())
	{
		Impl->JsonLogger = MakeUnique<FCamSimJsonLogger>();
		if (Impl->JsonLogger->Open(Config.Operational.StructuredLogPath,
		                            Config.Operational.StructuredLogMaxMB))
		{
			// Log startup config digest
			TMap<FString, FString> StartupFields;
			StartupFields.Add(TEXT("capture"), FString::Printf(TEXT("%dx%d"), Config.CaptureWidth, Config.CaptureHeight));
			StartupFields.Add(TEXT("codec"), Config.VideoCodec);
			StartupFields.Add(TEXT("bitrate"), FString::FromInt(Config.VideoBitrate));
			StartupFields.Add(TEXT("fps"), FString::SanitizeFloat(Config.FrameRate));
			Impl->JsonLogger->Log(TEXT("info"), TEXT("subsystem"), TEXT("initialized"), StartupFields);
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: failed to open structured log at %s"),
				*Config.Operational.StructuredLogPath);
			Impl->JsonLogger.Reset();
		}
	}
```

4. In the Tick() function, after the health file write block (around line 584), add:
```cpp
	// Phase 28B: flush structured log queue
	if (Impl->JsonLogger)
	{
		Impl->JsonLogger->Flush();
	}
```

5. In the `~FSubsystemImpl()` destructor, add early (before other resets):
```cpp
		if (JsonLogger) { JsonLogger->Close(); }
		JsonLogger.Reset();
```

6. In the shutdown delegate lambda, add:
```cpp
		if (Impl->JsonLogger)
		{
			TMap<FString, FString> ShutdownFields;
			ShutdownFields.Add(TEXT("frame"), FString::FromInt(Impl->FrameCntr));
			ShutdownFields.Add(TEXT("uptime_s"), FString::SanitizeFloat(
				FPlatformTime::Seconds() - Impl->StartTimeSec));
			Impl->JsonLogger->Log(TEXT("info"), TEXT("subsystem"), TEXT("shutdown"), ShutdownFields);
			Impl->JsonLogger->Flush();
		}
```

- [ ] **Step 19: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Logging/CamSimJsonLogger.h \
        unreal_project/CamSimTest/Source/CamSimTest/Logging/CamSimJsonLogger.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase28OpsTest.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp
git commit -m "feat(28B): add structured JSON logging sidecar with rotation"
```

---

## Chunk 3: Per-Frame Latency Tracking (28G)

### Task 6: Write latency tracker tests

**Files:**
- Modify: `Tests/Phase28OpsTest.cpp`

- [ ] **Step 20: Add latency tracker tests**

Append to `Phase28OpsTest.cpp`:

```cpp
// -------------------------------------------------------------------------
// Phase 28G: Pipeline Latency Tracker Tests
// -------------------------------------------------------------------------

#include "Diagnostics/PipelineLatencyTracker.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28LatencyFullBufferTest,
	"CamSim.Phase28.Latency.CommitAndPercentiles_FullBuffer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28LatencyFullBufferTest::RunTest(const FString& Parameters)
{
	FPipelineLatencyTracker Tracker(300);

	// Fill 300 frames with known deltas
	for (int32 i = 0; i < 300; ++i)
	{
		Tracker.SetStageTimestamp(EPipelineStage::CigiDequeue, static_cast<uint64>(i * 1000));
		Tracker.SetStageTimestamp(EPipelineStage::EncodeComplete, static_cast<uint64>(i * 1000 + 500));
		Tracker.CommitFrame();
	}

	FPipelineLatencyTracker::FLatencyPercentiles P = Tracker.ComputePercentiles();
	// All frames have same delta (500 cycles) — P50/P95/P99 should all be ~same
	TestTrue(TEXT("Total P50 > 0"), P.TotalUs[0] > 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28LatencyEmptyTest,
	"CamSim.Phase28.Latency.EmptyTracker_ReturnsZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28LatencyEmptyTest::RunTest(const FString& Parameters)
{
	FPipelineLatencyTracker Tracker(300);
	FPipelineLatencyTracker::FLatencyPercentiles P = Tracker.ComputePercentiles();
	TestEqual(TEXT("Empty P50 total is 0"), P.TotalUs[0], 0.0f);
	TestEqual(TEXT("Empty P95 total is 0"), P.TotalUs[1], 0.0f);
	TestEqual(TEXT("Empty P99 total is 0"), P.TotalUs[2], 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28LatencyPartialTest,
	"CamSim.Phase28.Latency.PartialBuffer_ComputesCorrectly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28LatencyPartialTest::RunTest(const FString& Parameters)
{
	FPipelineLatencyTracker Tracker(300);

	// Only 10 frames
	for (int32 i = 0; i < 10; ++i)
	{
		Tracker.SetStageTimestamp(EPipelineStage::CigiDequeue, static_cast<uint64>(i * 1000));
		Tracker.SetStageTimestamp(EPipelineStage::EncodeComplete, static_cast<uint64>(i * 1000 + 100 * (i + 1)));
		Tracker.CommitFrame();
	}

	FPipelineLatencyTracker::FLatencyPercentiles P = Tracker.ComputePercentiles();
	// P50 should be near median delta; P99 should be >= P50
	TestTrue(TEXT("Partial P50 > 0"), P.TotalUs[0] > 0.0f);
	TestTrue(TEXT("P99 >= P50"), P.TotalUs[2] >= P.TotalUs[0]);
	return true;
}
```

### Task 7: Implement FPipelineLatencyTracker

**Files:**
- Create: `Diagnostics/PipelineLatencyTracker.h`
- Create: `Diagnostics/PipelineLatencyTracker.cpp`

- [ ] **Step 21: Create PipelineLatencyTracker.h**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

/** Pipeline stages for latency tracking. */
enum class EPipelineStage : uint8
{
	CigiDequeue = 0,
	GameTickStart,
	ReadbackIssue,
	ReadbackComplete,
	SensorStart,
	SensorEnd,
	EncodeComplete,
	Count
};

/**
 * FPipelineLatencyTracker
 *
 * Records per-frame timestamps at each pipeline stage and computes
 * P50/P95/P99 latency percentiles over a ring buffer.
 *
 * Thread safety: Multiple threads write to the current frame's staging area
 * via SetStageTimestamp() (each stage written by exactly one thread — no
 * contention on the same slot). CommitFrame() atomically advances the write
 * index. ComputePercentiles() reads committed entries (no lock needed — reader
 * only touches entries behind the write index).
 */
struct FPipelineLatencyTracker
{
	explicit FPipelineLatencyTracker(int32 BufferSize = 300);

	/** Record a timestamp for the given pipeline stage in the current frame. */
	void SetStageTimestamp(EPipelineStage Stage, uint64 Cycles);

	/** Convenience: record current time for the given stage. */
	void Mark(EPipelineStage Stage) { SetStageTimestamp(Stage, FPlatformTime::Cycles64()); }

	/** Commit the current frame to the ring buffer. Call once per frame from encoder thread. */
	void CommitFrame();

	struct FLatencyPercentiles
	{
		float ReadbackUs[3] = {};  // P50, P95, P99
		float SensorUs[3]   = {};
		float EncodeUs[3]   = {};
		float TotalUs[3]    = {};  // CigiDequeue → EncodeComplete
	};

	/** Compute percentiles over committed frames. Safe to call from game thread. */
	FLatencyPercentiles ComputePercentiles() const;

	/** Number of committed frames in the buffer. */
	int32 GetCommittedCount() const;

private:
	struct FLatencyRecord
	{
		uint64 Stages[static_cast<int32>(EPipelineStage::Count)] = {};
	};

	TArray<FLatencyRecord> Records;
	FLatencyRecord CurrentFrame;
	TAtomic<uint32> WriteIndex { 0 };
	int32 BufferCapacity = 300;
	TAtomic<int32> CommittedCount { 0 };

	static float CyclesToUs(uint64 Delta);
	static float PercentileFromSorted(const TArray<float>& Sorted, float Pct);
};
```

- [ ] **Step 22: Create PipelineLatencyTracker.cpp**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Diagnostics/PipelineLatencyTracker.h"

FPipelineLatencyTracker::FPipelineLatencyTracker(int32 InBufferSize)
	: BufferCapacity(FMath::Max(10, InBufferSize))
{
	Records.SetNum(BufferCapacity);
	FMemory::Memzero(CurrentFrame);
}

void FPipelineLatencyTracker::SetStageTimestamp(EPipelineStage Stage, uint64 Cycles)
{
	CurrentFrame.Stages[static_cast<int32>(Stage)] = Cycles;
}

void FPipelineLatencyTracker::CommitFrame()
{
	const uint32 Idx = WriteIndex.Load(EMemoryOrder::Relaxed);
	Records[Idx % BufferCapacity] = CurrentFrame;
	WriteIndex.Store(Idx + 1, EMemoryOrder::Relaxed);

	const int32 Count = CommittedCount.Load(EMemoryOrder::Relaxed);
	if (Count < BufferCapacity)
	{
		CommittedCount.Store(Count + 1, EMemoryOrder::Relaxed);
	}

	FMemory::Memzero(CurrentFrame);
}

int32 FPipelineLatencyTracker::GetCommittedCount() const
{
	return CommittedCount.Load(EMemoryOrder::Relaxed);
}

float FPipelineLatencyTracker::CyclesToUs(uint64 Delta)
{
	return static_cast<float>(FPlatformTime::ToSeconds64(Delta) * 1000000.0);
}

float FPipelineLatencyTracker::PercentileFromSorted(const TArray<float>& Sorted, float Pct)
{
	if (Sorted.Num() == 0) return 0.0f;
	const int32 Idx = FMath::Clamp(
		static_cast<int32>(Pct * Sorted.Num()),
		0, Sorted.Num() - 1);
	return Sorted[Idx];
}

FPipelineLatencyTracker::FLatencyPercentiles FPipelineLatencyTracker::ComputePercentiles() const
{
	FLatencyPercentiles Result;
	const int32 Count = CommittedCount.Load(EMemoryOrder::Relaxed);
	if (Count == 0) return Result;

	// Collect deltas
	TArray<float> ReadbackDeltas, SensorDeltas, EncodeDeltas, TotalDeltas;
	ReadbackDeltas.Reserve(Count);
	SensorDeltas.Reserve(Count);
	EncodeDeltas.Reserve(Count);
	TotalDeltas.Reserve(Count);

	const uint32 WIdx = WriteIndex.Load(EMemoryOrder::Relaxed);
	const int32 StartIdx = (Count < BufferCapacity) ? 0 : static_cast<int32>(WIdx % BufferCapacity);

	for (int32 i = 0; i < Count; ++i)
	{
		const FLatencyRecord& R = Records[(StartIdx + i) % BufferCapacity];

		const uint64 Readback = R.Stages[static_cast<int32>(EPipelineStage::ReadbackComplete)];
		const uint64 RbIssue  = R.Stages[static_cast<int32>(EPipelineStage::ReadbackIssue)];
		const uint64 SensorS  = R.Stages[static_cast<int32>(EPipelineStage::SensorStart)];
		const uint64 SensorE  = R.Stages[static_cast<int32>(EPipelineStage::SensorEnd)];
		const uint64 EncComp  = R.Stages[static_cast<int32>(EPipelineStage::EncodeComplete)];
		const uint64 CigiDq   = R.Stages[static_cast<int32>(EPipelineStage::CigiDequeue)];

		if (Readback > RbIssue)  ReadbackDeltas.Add(CyclesToUs(Readback - RbIssue));
		if (SensorE > SensorS)   SensorDeltas.Add(CyclesToUs(SensorE - SensorS));
		if (EncComp > SensorE)   EncodeDeltas.Add(CyclesToUs(EncComp - SensorE));
		if (EncComp > CigiDq)    TotalDeltas.Add(CyclesToUs(EncComp - CigiDq));
	}

	ReadbackDeltas.Sort();
	SensorDeltas.Sort();
	EncodeDeltas.Sort();
	TotalDeltas.Sort();

	const float Pcts[] = { 0.50f, 0.95f, 0.99f };
	for (int32 p = 0; p < 3; ++p)
	{
		Result.ReadbackUs[p] = PercentileFromSorted(ReadbackDeltas, Pcts[p]);
		Result.SensorUs[p]   = PercentileFromSorted(SensorDeltas, Pcts[p]);
		Result.EncodeUs[p]   = PercentileFromSorted(EncodeDeltas, Pcts[p]);
		Result.TotalUs[p]    = PercentileFromSorted(TotalDeltas, Pcts[p]);
	}

	return Result;
}
```

- [ ] **Step 23: Build and run latency tracker tests**

All 3 latency tests should pass.

- [ ] **Step 24: Wire tracker into Camera and EncoderThread**

In `Camera/CamSimCamera.h`, add:
```cpp
#include "Diagnostics/PipelineLatencyTracker.h"
```
And in the private section:
```cpp
	/** Phase 28G: per-frame pipeline latency tracker (owned by subsystem, nullable). */
	FPipelineLatencyTracker* LatencyTracker_ = nullptr;
```

Add a public setter:
```cpp
	void SetLatencyTracker(FPipelineLatencyTracker* Tracker) { LatencyTracker_ = Tracker; }
```

In `Camera/CamSimCamera.cpp`, add Mark calls at each stage point:
- Top of `Tick()`: `if (LatencyTracker_) LatencyTracker_->Mark(EPipelineStage::GameTickStart);`
- After dequeue of CIGI entity state: `if (LatencyTracker_) LatencyTracker_->Mark(EPipelineStage::CigiDequeue);`
- Before `ENQUEUE_RENDER_COMMAND`: `if (LatencyTracker_) LatencyTracker_->Mark(EPipelineStage::ReadbackIssue);`
- After `FlushRenderingCommands()` returns: `if (LatencyTracker_) LatencyTracker_->Mark(EPipelineStage::ReadbackComplete);`
- Before sensor post-process: `if (LatencyTracker_) LatencyTracker_->Mark(EPipelineStage::SensorStart);`
- After sensor post-process: `if (LatencyTracker_) LatencyTracker_->Mark(EPipelineStage::SensorEnd);`

In `Encoder/EncoderThread.h`, add:
```cpp
class FPipelineLatencyTracker;
```
And a member + setter:
```cpp
	FPipelineLatencyTracker* LatencyTracker = nullptr;
	void SetLatencyTracker(FPipelineLatencyTracker* Tracker) { LatencyTracker = Tracker; }
```

In `Encoder/EncoderThread.cpp`, in the `Run()` loop after `Encoder->EncodeFrame(...)` (line 82), add:
```cpp
			if (LatencyTracker)
			{
				LatencyTracker->Mark(EPipelineStage::EncodeComplete);
				LatencyTracker->CommitFrame();
			}
```

Add include: `#include "Diagnostics/PipelineLatencyTracker.h"`

- [ ] **Step 25: Wire tracker into Subsystem and health/prometheus output**

In `Subsystem/CamSimSubsystem.cpp`:

1. Add include: `#include "Diagnostics/PipelineLatencyTracker.h"`

2. Add to `FSubsystemImpl`:
```cpp
	TUniquePtr<FPipelineLatencyTracker> LatencyTracker;
```

3. In `Initialize()`, after JsonLogger init:
```cpp
	// Phase 28G: pipeline latency tracker
	if (Config.Performance.bTrackPipelineLatency)
	{
		const int32 BufSize = static_cast<int32>(Config.Performance.OutputFrameRateHz * 10.0f);
		Impl->LatencyTracker = MakeUnique<FPipelineLatencyTracker>(BufSize);
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: pipeline latency tracking enabled (buffer=%d)"), BufSize);
	}
```

4. In the camera registration code (where `SetLatencyTracker` can be called), after the camera is registered:
```cpp
	if (Impl->LatencyTracker && Camera_.IsValid())
	{
		Camera_->SetLatencyTracker(Impl->LatencyTracker.Get());
	}
```

5. Similarly for the encoder thread — after encoder thread Start():
```cpp
	if (Impl->LatencyTracker)
	{
		// The encoder thread references the tracker for CommitFrame
		// Set it after the FEncoderThread is created in ACamSimCamera::BeginPlay
	}
```

6. In the health JSON write block (after frame_drops), add:
```cpp
		// Phase 28G: pipeline latency percentiles
		if (Impl->LatencyTracker)
		{
			auto P = Impl->LatencyTracker->ComputePercentiles();
			HealthJson += FString::Printf(
				TEXT(",\"latency_us\":{\"readback_p50\":%.0f,\"readback_p95\":%.0f,\"readback_p99\":%.0f,")
				TEXT("\"sensor_p50\":%.0f,\"sensor_p95\":%.0f,\"sensor_p99\":%.0f,")
				TEXT("\"encode_p50\":%.0f,\"encode_p95\":%.0f,\"encode_p99\":%.0f,")
				TEXT("\"total_p50\":%.0f,\"total_p95\":%.0f,\"total_p99\":%.0f}"),
				P.ReadbackUs[0], P.ReadbackUs[1], P.ReadbackUs[2],
				P.SensorUs[0], P.SensorUs[1], P.SensorUs[2],
				P.EncodeUs[0], P.EncodeUs[1], P.EncodeUs[2],
				P.TotalUs[0], P.TotalUs[1], P.TotalUs[2]);
		}
```

7. In the Prometheus write block, add new metrics after existing ones:
```cpp
		if (Impl->LatencyTracker)
		{
			auto P = Impl->LatencyTracker->ComputePercentiles();
			Prom += FString::Printf(
				TEXT("# HELP camsim_latency_readback_us Readback latency microseconds\n"
				     "# TYPE camsim_latency_readback_us gauge\n"
				     "camsim_latency_readback_us{quantile=\"0.5\"} %.0f\n"
				     "camsim_latency_readback_us{quantile=\"0.95\"} %.0f\n"
				     "camsim_latency_readback_us{quantile=\"0.99\"} %.0f\n"
				     "# HELP camsim_latency_sensor_us Sensor pipeline latency microseconds\n"
				     "# TYPE camsim_latency_sensor_us gauge\n"
				     "camsim_latency_sensor_us{quantile=\"0.5\"} %.0f\n"
				     "camsim_latency_sensor_us{quantile=\"0.95\"} %.0f\n"
				     "camsim_latency_sensor_us{quantile=\"0.99\"} %.0f\n"
				     "# HELP camsim_latency_encode_us Encode latency microseconds\n"
				     "# TYPE camsim_latency_encode_us gauge\n"
				     "camsim_latency_encode_us{quantile=\"0.5\"} %.0f\n"
				     "camsim_latency_encode_us{quantile=\"0.95\"} %.0f\n"
				     "camsim_latency_encode_us{quantile=\"0.99\"} %.0f\n"
				     "# HELP camsim_latency_total_us Total pipeline latency microseconds\n"
				     "# TYPE camsim_latency_total_us gauge\n"
				     "camsim_latency_total_us{quantile=\"0.5\"} %.0f\n"
				     "camsim_latency_total_us{quantile=\"0.95\"} %.0f\n"
				     "camsim_latency_total_us{quantile=\"0.99\"} %.0f\n"),
				P.ReadbackUs[0], P.ReadbackUs[1], P.ReadbackUs[2],
				P.SensorUs[0], P.SensorUs[1], P.SensorUs[2],
				P.EncodeUs[0], P.EncodeUs[1], P.EncodeUs[2],
				P.TotalUs[0], P.TotalUs[1], P.TotalUs[2]);
		}
```

- [ ] **Step 26: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.h \
        unreal_project/CamSimTest/Source/CamSimTest/Diagnostics/PipelineLatencyTracker.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/Phase28OpsTest.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.h \
        unreal_project/CamSimTest/Source/CamSimTest/Camera/CamSimCamera.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Encoder/EncoderThread.h \
        unreal_project/CamSimTest/Source/CamSimTest/Encoder/EncoderThread.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp
git commit -m "feat(28G): add per-frame pipeline latency tracking with P50/P95/P99"
```

---

## Chunk 4: HTTP Health Endpoints (28C)

### Task 8: Create HTTP health server

**Files:**
- Modify: `CamSimTest.Build.cs` — add `"HTTPServer"`
- Create: `Health/CamSimHealthServer.h`
- Create: `Health/CamSimHealthServer.cpp`

- [ ] **Step 27: Add HTTPServer module to Build.cs**

In `CamSimTest.Build.cs`, add `"HTTPServer"` to `PublicDependencyModuleNames` (after `"NiagaraCore"`):

```csharp
			// HTTP health endpoints (Phase 28C)
			"HTTPServer",
```

- [ ] **Step 28: Create CamSimHealthServer.h**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class IHttpRouter;

/**
 * FCamSimHealthServer
 *
 * Lightweight HTTP server for Kubernetes liveness/readiness probes.
 * Uses UE5's FHttpServerModule (IHttpRouter).
 *
 * Routes:
 *   GET /live    — 200 if game loop ticked within 5s
 *   GET /ready   — 200 if encoder + CIGI + first frame all OK
 *   GET /metrics — Prometheus exposition format
 */
struct FCamSimHealthServer
{
	using FStatusQueryFn = TFunction<bool()>;

	/** Start the HTTP server on the given port. */
	bool Start(int32 Port,
	           FStatusQueryFn InIsAlive,
	           FStatusQueryFn InIsEncoderReady,
	           FStatusQueryFn InIsCigiReady,
	           FStatusQueryFn InHasFirstFrame,
	           TFunction<FString()> InGetPrometheusMetrics);

	/** Stop the HTTP server. */
	void Stop();

	/** Call from game thread tick to update the liveness timestamp. */
	void UpdateTick();

private:
	TSharedPtr<IHttpRouter> Router;
	double LastTickTimeSec = 0.0;
	int32 ListenPort = 0;

	FStatusQueryFn IsAlive;
	FStatusQueryFn IsEncoderReady;
	FStatusQueryFn IsCigiReady;
	FStatusQueryFn HasFirstFrame;
	TFunction<FString()> GetPrometheusMetrics;
};
```

- [ ] **Step 29: Create CamSimHealthServer.cpp**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "Health/CamSimHealthServer.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpResultCallback.h"
#include "CamSimTest.h"

bool FCamSimHealthServer::Start(int32 Port,
                                FStatusQueryFn InIsAlive,
                                FStatusQueryFn InIsEncoderReady,
                                FStatusQueryFn InIsCigiReady,
                                FStatusQueryFn InHasFirstFrame,
                                TFunction<FString()> InGetPrometheusMetrics)
{
	IsAlive = MoveTemp(InIsAlive);
	IsEncoderReady = MoveTemp(InIsEncoderReady);
	IsCigiReady = MoveTemp(InIsCigiReady);
	HasFirstFrame = MoveTemp(InHasFirstFrame);
	GetPrometheusMetrics = MoveTemp(InGetPrometheusMetrics);
	ListenPort = Port;
	LastTickTimeSec = FPlatformTime::Seconds();

	Router = FHttpServerModule::Get().GetHttpRouter(Port);
	if (!Router)
	{
		UE_LOG(LogCamSim, Error, TEXT("FCamSimHealthServer: failed to get HTTP router on port %d"), Port);
		return false;
	}

	// GET /live
	Router->BindRoute(FHttpPath(TEXT("/live")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
		{
			const double AgeSec = FPlatformTime::Seconds() - LastTickTimeSec;
			if (AgeSec < 5.0)
			{
				auto Response = FHttpServerResponse::Create(TEXT("{\"status\":\"ok\"}"), TEXT("application/json"));
				OnComplete(MoveTemp(Response));
			}
			else
			{
				FString Body = FString::Printf(TEXT("{\"status\":\"stalled\",\"last_tick_ago_s\":%.1f}"), AgeSec);
				auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
				OnComplete(MoveTemp(Response));
			}
			return true;
		}));

	// GET /ready
	Router->BindRoute(FHttpPath(TEXT("/ready")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
		{
			const bool bEncoder = IsEncoderReady ? IsEncoderReady() : false;
			const bool bCigi = IsCigiReady ? IsCigiReady() : false;
			const bool bFrame = HasFirstFrame ? HasFirstFrame() : false;
			const bool bReady = bEncoder && bCigi && bFrame;

			FString Body = FString::Printf(
				TEXT("{\"status\":\"%s\",\"encoder\":%s,\"cigi\":%s,\"first_frame\":%s}"),
				bReady ? TEXT("ready") : TEXT("not_ready"),
				bEncoder ? TEXT("true") : TEXT("false"),
				bCigi ? TEXT("true") : TEXT("false"),
				bFrame ? TEXT("true") : TEXT("false"));

			auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
			OnComplete(MoveTemp(Response));
			return true;
		}));

	// GET /metrics
	Router->BindRoute(FHttpPath(TEXT("/metrics")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
		{
			FString Body = GetPrometheusMetrics ? GetPrometheusMetrics() : TEXT("");
			auto Response = FHttpServerResponse::Create(Body, TEXT("text/plain; charset=utf-8"));
			OnComplete(MoveTemp(Response));
			return true;
		}));

	FHttpServerModule::Get().StartAllListeners();
	UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: listening on port %d (/live /ready /metrics)"), Port);
	return true;
}

void FCamSimHealthServer::Stop()
{
	if (Router)
	{
		FHttpServerModule::Get().StopAllListeners();
		Router.Reset();
		UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: stopped"));
	}
}

void FCamSimHealthServer::UpdateTick()
{
	LastTickTimeSec = FPlatformTime::Seconds();
}
```

- [ ] **Step 30: Wire health server into Subsystem**

In `Subsystem/CamSimSubsystem.cpp`:

1. Add include: `#include "Health/CamSimHealthServer.h"`

2. Add to `FSubsystemImpl`:
```cpp
	TUniquePtr<FCamSimHealthServer> HealthServer;
```

3. In `Initialize()`, after latency tracker init:
```cpp
	// Phase 28C: HTTP health endpoints
	if (Config.Operational.bHealthHttpEnabled)
	{
		Impl->HealthServer = MakeUnique<FCamSimHealthServer>();
		auto* ImplPtr = Impl.Get();
		Impl->HealthServer->Start(Config.Operational.HealthHttpPort,
			// IsAlive
			[ImplPtr]() { return true; },
			// IsEncoderReady
			[ImplPtr]() { return ImplPtr->VideoEncoder && ImplPtr->VideoEncoder->IsOpen(); },
			// IsCigiReady
			[ImplPtr]() { return ImplPtr->CigiReceiver && ImplPtr->CigiReceiver->IsRunning(); },
			// HasFirstFrame
			[ImplPtr]() { return ImplPtr->VideoEncoder && ImplPtr->VideoEncoder->GetSuccessfulFrameCount() > 0; },
			// GetPrometheusMetrics — returns same format as textfile
			[this]() -> FString { return GeneratePrometheusMetrics(); }
		);
	}
```

Note: `GeneratePrometheusMetrics()` should be extracted from the existing Prometheus write block into a shared helper method to avoid code duplication. Add a private method to `UCamSimSubsystem`:
```cpp
FString UCamSimSubsystem::GeneratePrometheusMetrics() const;
```

4. In `Tick()`, near the top, add:
```cpp
	if (Impl->HealthServer)
	{
		Impl->HealthServer->UpdateTick();
	}
```

5. In `~FSubsystemImpl()`, add before JsonLogger cleanup:
```cpp
		if (HealthServer) { HealthServer->Stop(); }
		HealthServer.Reset();
```

- [ ] **Step 31: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.h \
        unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h
git commit -m "feat(28C): add HTTP health endpoints (/live /ready /metrics)"
```

---

## Chunk 5: Unit Tests in CI (28A)

### Task 9: Add unit-tests CI job

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 32: Add unit-tests job to ci.yml**

Add after the `docker-build` job (before `integration-test`):

```yaml
  # -------------------------------------------------------------------
  # Phase 28A: UE5 automation tests (headless Docker)
  # -------------------------------------------------------------------
  unit-tests:
    name: UE5 Unit Tests
    runs-on: ubuntu-latest
    needs: [docker-build]
    steps:
      - uses: actions/checkout@v4

      - name: Build Docker image
        uses: docker/build-push-action@v5
        with:
          context: deploy/
          push: false
          load: true
          tags: camsim:test-${{ github.sha }}
          cache-from: type=gha

      - name: Run UE5 automation tests
        run: |
          docker run --rm --name camsim-unit-tests \
            -e CAMSIM_CIGI_PORT=0 \
            camsim:test-${{ github.sha }} \
            /opt/camsim/CamSimTest/Binaries/Linux/UnrealEditor-Cmd \
            /opt/camsim/CamSimTest/CamSimTest.uproject \
            -ExecCmds="Automation RunAll; Quit" \
            -NullRHI -NoSound -Unattended \
            -log 2>&1 | tee /tmp/ue5-test-output.txt
          # Check for test failures in output
          if grep -q "Test Completed. Result={Fail}" /tmp/ue5-test-output.txt; then
            echo "::error::UE5 automation tests failed"
            exit 1
          fi
          if grep -q "LogAutomationController.*Fail" /tmp/ue5-test-output.txt; then
            echo "::error::UE5 automation tests failed"
            exit 1
          fi
          echo "All UE5 automation tests passed"
        timeout-minutes: 10

      - name: Upload test log
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: ue5-test-log
          path: /tmp/ue5-test-output.txt
```

- [ ] **Step 33: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "feat(28A): add UE5 unit tests to CI pipeline"
```

---

## Chunk 6: Final Integration & Plan.md Update

### Task 10: Update Plan.md and documentation

**Files:**
- Modify: `Plan.md`

- [ ] **Step 34: Update Phase 28 in Plan.md**

Mark Sprint 1 items as done in the Phase 28 table:

```
| **28A** Unit Tests in CI           | Run UE automation tests in GitHub Actions                     | M      | ✅ Sprint 1 Done |
| **28B** Structured JSON Logging    | Machine-parseable JSONL sidecar for ELK/Datadog              | M      | ✅ Sprint 1 Done |
| **28C** HTTP Health Endpoints      | /live and /ready for Kubernetes probes                        | M      | ✅ Sprint 1 Done |
| **28D** Config Validation          | Pre-flight range checks on all config values                  | S      | ✅ Sprint 1 Done |
| **28G** Per-Frame Latency Tracking | Pipeline stage timestamps; P50/P95/P99 to Prometheus          | M      | ✅ Sprint 1 Done |
```

Add Sprint 1 files listing and validation section.

- [ ] **Step 35: Run all tests**

Build the project and run the full automation test suite to verify no regressions. All existing 163+ tests plus the 10 new Phase 28 tests should pass.

- [ ] **Step 36: Final commit**

```bash
git add Plan.md
git commit -m "docs: update Plan.md for Phase 28 Sprint 1 completion"
```

---

## Verification Checklist

After all tasks complete:

1. **28D**: Modify `camsim_config.yaml` to set `multicast_port: 99999`. Start CamSim. Verify log shows "MulticastPort=99999 out of range [1, 65535]" and startup aborts with `bLoadedSuccessfully=false`.

2. **28B**: Set env `CAMSIM_STRUCTURED_LOG_PATH=/tmp/camsim.jsonl`. Run CamSim briefly. Verify `cat /tmp/camsim.jsonl | jq .` shows valid JSON lines with `ts`, `severity`, `category`, `msg` fields.

3. **28G**: Set env `CAMSIM_TRACK_PIPELINE_LATENCY=1`. Run CamSim for 30+ seconds. Check `camsim_health.json` includes `latency_us` block with readback/sensor/encode/total percentiles.

4. **28C**: Set env `CAMSIM_HEALTH_HTTP_ENABLED=1`. Run CamSim. `curl localhost:8080/live` → `{"status":"ok"}`. `curl localhost:8080/ready` → shows encoder/cigi/first_frame status. `curl localhost:8080/metrics` → Prometheus format.

5. **28A**: Push to a PR branch. Verify `UE5 Unit Tests` job appears in GitHub Actions and passes.
