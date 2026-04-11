# camera-simulator HTTP Health & Metrics Endpoint — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an HTTP server to camera-simulator exposing `/health`, `/ready`, and `/metrics` on a configurable port (default 8910), so the sim-environment REST orchestrator can probe camsim health uniformly whether camsim runs in Docker or on the bare host.

**Architecture:** Use UE5's built-in `HTTPServer` runtime module (`FHttpServerModule` + `IHttpRouter`). A new `FCamSimHttpServer` class owned by `UCamSimSubsystem` registers three GET routes. Route handlers read atomic counters from the existing Pimpl state and serialize them via pure-function formatters (`HealthJson`, `PrometheusFormatter`) that are unit-testable without a live HTTP server. The existing file-based `camsim_health.json` and `.prom` textfile paths stay in place for backward compatibility.

**Tech Stack:** UE5 C++ (existing project), `HTTPServer` runtime module, UE5 Automation Framework for tests, ryml for YAML config, existing `FCamSimConfig` patterns.

**Source spec:** `/opt/mac/sim-environment/sim-environment/docs/superpowers/specs/2026-04-10-dotnet-rest-orchestrator-design.md` §10.

---

## Conventions for this plan

- All paths are **relative to** `/opt/mac/sim-environment/camera-simulator/`.
- All file creation uses the header `// Copyright CamSim Contributors. All Rights Reserved.` per the project's established style (see `Source/CamSimTest/Tests/CamSimConfigTest.cpp:1`).
- All new files live under `Source/CamSimTest/HttpServer/` except tests which go in `Source/CamSimTest/Tests/`.
- After code changes, run `scripts/check.sh` before each commit to catch compile errors early.
- Automation tests follow the `IMPLEMENT_SIMPLE_AUTOMATION_TEST` pattern with flags `EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter` and test category `CamSim.HttpServer.*`.
- Commits are small and frequent — one per task unless a task is a single config addition without code.

---

## File Structure

**Files created:**

| Path | Purpose |
|---|---|
| `Source/CamSimTest/HttpServer/FCamSimHttpServer.h` | Lifecycle wrapper owning `FHttpServerModule` routes. Constructed/destroyed by `UCamSimSubsystem`. |
| `Source/CamSimTest/HttpServer/FCamSimHttpServer.cpp` | Route registration, request dispatch, subsystem pointer plumbing. |
| `Source/CamSimTest/HttpServer/HealthSnapshot.h` | Plain POD struct holding all counters needed for `/health`, `/ready`, `/metrics`. |
| `Source/CamSimTest/HttpServer/HealthSnapshot.cpp` | `HealthSnapshot::CaptureFrom(subsystem)` — reads atomic counters into a snapshot. |
| `Source/CamSimTest/HttpServer/HealthJson.h` | Pure-function `BuildHealthJson(snapshot)` and `BuildReadyJson(snapshot, bReady, reason)`. |
| `Source/CamSimTest/HttpServer/HealthJson.cpp` | JSON serialization implementation. |
| `Source/CamSimTest/HttpServer/PrometheusFormatter.h` | Pure-function `BuildPrometheusText(snapshot)`. |
| `Source/CamSimTest/HttpServer/PrometheusFormatter.cpp` | Prometheus text format emitter. |
| `Source/CamSimTest/HttpServer/ReadinessEvaluator.h` | Pure function `EvaluateReadiness(snapshot, bootstrapGracePeriodSec) → { bReady, Reason }`. |
| `Source/CamSimTest/HttpServer/ReadinessEvaluator.cpp` | Readiness logic. |
| `Source/CamSimTest/Tests/HttpServerHealthJsonTest.cpp` | Unit tests for `BuildHealthJson` / `BuildReadyJson`. |
| `Source/CamSimTest/Tests/HttpServerPrometheusTest.cpp` | Unit tests for `BuildPrometheusText`. |
| `Source/CamSimTest/Tests/HttpServerReadinessTest.cpp` | Unit tests for `EvaluateReadiness`. |
| `Source/CamSimTest/Tests/HttpServerLifecycleTest.cpp` | Integration test: start server, GET /health via client, stop server. |

**Files modified:**

| Path | Change |
|---|---|
| `unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs` | Add `"HTTPServer"` and `"HTTP"` to `PublicDependencyModuleNames`. |
| `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h` | Add `FHttpServerConfig` struct and `FHttpServerConfig HttpServer;` field to `FCamSimConfig`. |
| `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp` | Add YAML parse block and env var overrides for `http_server.*`. |
| `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h` | Forward-declare `FCamSimHttpServer` and add `TUniquePtr<FCamSimHttpServer> HttpServer` member inside `FSubsystemImpl`. |
| `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` | Construct/start `FCamSimHttpServer` in `Initialize()`, teardown in `FSubsystemImpl` destructor. |
| `deploy/camsim_config.yaml` | Add new `http_server:` block with comments matching file style. |
| `docs/configuration.md` | Document new `http_server` fields and env vars. |
| `CLAUDE.md` | Update architecture section, port table, environment list, "Health" gotcha. |
| `Plan.md` | Add new phase entry and mark Phase 28G JSON health endpoint as in-progress. |

---

## Dependency notes for the engineer

**UE5 `HTTPServer` module — quick reference:**
- Located at `Engine/Source/Runtime/Online/HTTPServer/`. Ships with the engine; no .uproject plugin enable needed.
- Enable by adding `"HTTPServer"` to `PublicDependencyModuleNames` in `CamSimTest.Build.cs`.
- Key types (all in `HttpServerModule.h`, `HttpRouteHandle.h`, `HttpServerRequest.h`, `HttpServerResponse.h`, `IHttpRouter.h`):
  - `FHttpServerModule::Get()` — module singleton
  - `FHttpServerModule::Get().GetHttpRouter(uint32 Port)` returns `TSharedPtr<IHttpRouter>`
  - `IHttpRouter::BindRoute(const FHttpPath&, EHttpServerRequestVerbs, const FHttpRequestHandler&)` returns `FHttpRouteHandle`
  - `FHttpRequestHandler` is `TUniqueFunction<bool(const FHttpServerRequest&, const FHttpResultCallback&)>`
  - `FHttpServerResponse::Create(FString Body, FString ContentType)` for plain responses
  - `FHttpServerModule::Get().StartAllListeners()` begins listening on bound ports
  - `FHttpServerModule::Get().StopAllListeners()` stops them

**`"HTTP"` module** — needed only by the integration test to make client requests against the running server. Provides `FHttpModule::Get().CreateRequest()`.

**Thread model:** route handlers are invoked off the game thread by the HTTPServer module's own worker. They must NOT touch `UObject`s without a game-thread marshal. Our handlers only read POD counters via atomic loads — no `UObject` access — so no marshalling needed.

**Counters we read** (all from `FSubsystemImpl` in `Subsystem/CamSimSubsystem.cpp:54-82`):
- `Impl->FrameCntr` (uint32, game-thread write; we read as a snapshot via `FPlatformAtomics::AtomicRead_Relaxed` — see Task 4)
- `Impl->WatchdogReconnectCount` (uint32)
- `Impl->StartTimeSec` (double, set once in Initialize)
- `Impl->IGMode` (uint8)
- `Encoder->IsOpen()` (bool)
- `Encoder->GetSuccessfulFrameCount()` (uint64)
- `Impl->CigiReceiver->GetReceivedPacketCount()` (uint64)
- `Impl->CigiReceiver->GetLastHostFrame()` (uint32)

These match the fields already captured by the existing `camsim_health.json` writer at `Subsystem/CamSimSubsystem.cpp:580-611` and the existing Prometheus file writer at `Subsystem/CamSimSubsystem.cpp:614-658`. The HTTP endpoints expose the same information via a different transport.

---

## Task 1: Enable HTTPServer + HTTP modules in the build

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs:21-47`

- [ ] **Step 1: Add `"HTTPServer"` and `"HTTP"` to `PublicDependencyModuleNames`**

Edit `CamSimTest.Build.cs`. In the `PublicDependencyModuleNames.AddRange` block, add two entries after `"Networking"`:

```csharp
PublicDependencyModuleNames.AddRange(new string[]
{
    "Core",
    "CoreUObject",
    "Engine",
    "InputCore",
    "EnhancedInput",
    // Rendering
    "RenderCore",
    "RHI",
    "Renderer",
    // Networking
    "Sockets",
    "Networking",
    // HTTP server for /health, /ready, /metrics (Phase 28G)
    "HTTPServer",
    // HTTP client for integration tests of the HTTP server
    "HTTP",
    // Cesium
    "CesiumRuntime",
    // JSON config
    "Json",
    "JsonUtilities",
    // Runtime glTF loading for entity meshes
    "glTFRuntime",
    // PNG encoding for depth maps (Phase 17A)
    "ImageWrapper",
    // Niagara particle FX (Phase 18F/G/H/I)
    "Niagara",
    "NiagaraCore",
});
```

- [ ] **Step 2: Verify the build still compiles with the new module deps**

Run: `scripts/check.sh`
Expected: exit code 0, no new errors. UBT may take several minutes on a cold build as it pulls in the HTTPServer and HTTP module headers.

- [ ] **Step 3: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs
git commit -m "$(cat <<'EOF'
build: add HTTPServer and HTTP module deps

Needed for Phase 28G HTTP health endpoint work. HTTPServer exposes
the embedded REST server; HTTP is used by automation tests to make
client requests against the running server.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Add `FHttpServerConfig` struct to `FCamSimConfig`

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h` (add struct and field near the Prometheus block around line 365)

- [ ] **Step 1: Add the struct definition and the field to `FCamSimConfig`**

Open `CamSimConfig.h`. Find the block near line 365 that starts with `// Prometheus metrics file path (empty = disabled). Phase 12D.`. Insert a new struct and field *immediately after* the `FString PrometheusMetricsPath;` line and before `// Recording & Playback (Phase 12E)`:

```cpp
	// Prometheus metrics file path (empty = disabled). Phase 12D.
	// A Prometheus node_exporter textfile-collector compatible .prom file.
	FString PrometheusMetricsPath;

	// HTTP health & metrics server (Phase 28G).
	// Exposes /health, /ready, and /metrics on a dedicated port. Used by the
	// sim-environment orchestrator's HealthMonitor and (optionally) by Grafana
	// for direct Prometheus scraping as a replacement for the textfile-collector
	// path above. Both can run simultaneously; the HTTP path is additive.
	//
	// Env vars:
	//   CAMSIM_HTTP_SERVER_ENABLED — 0/1 master toggle            (default 1)
	//   CAMSIM_HTTP_BIND_ADDR      — interface bind address       (default 0.0.0.0)
	//   CAMSIM_HTTP_PORT           — listen port                  (default 8910)
	struct FHttpServerConfig
	{
		bool    bEnabled = true;
		FString BindAddr = TEXT("0.0.0.0");
		int32   Port     = 8910;
	};
	FHttpServerConfig HttpServer;

	// Recording & Playback (Phase 12E)
```

- [ ] **Step 2: Verify the build still compiles**

Run: `scripts/check.sh`
Expected: exit code 0.

- [ ] **Step 3: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h
git commit -m "$(cat <<'EOF'
config: add FHttpServerConfig struct to FCamSimConfig

Phase 28G groundwork. Adds the config field that will be populated by
YAML parse + env var overrides in the next commit. No runtime effect
yet — the HTTP server itself is wired up in a later commit.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add YAML parsing and env var overrides for `http_server`

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp` (YAML block near line 873, env var block near line 1399)

- [ ] **Step 1: Add YAML parse block**

In `CamSimConfig.cpp`, find the block near line 873 that reads:

```cpp
		// Prometheus metrics (Phase 12D)
		YamlString(Root, "prometheus_metrics_path", Cfg.PrometheusMetricsPath);
```

Insert immediately after it:

```cpp
		// Prometheus metrics (Phase 12D)
		YamlString(Root, "prometheus_metrics_path", Cfg.PrometheusMetricsPath);

		// HTTP health & metrics server (Phase 28G)
		if (Root.has_child("http_server"))
		{
			ryml::ConstNodeRef HttpNode = Root["http_server"];
			YamlBool  (HttpNode, "enabled",   Cfg.HttpServer.bEnabled);
			YamlString(HttpNode, "bind_addr", Cfg.HttpServer.BindAddr);
			YamlInt   (HttpNode, "port",      Cfg.HttpServer.Port);
		}
```

- [ ] **Step 2: Add env var override block**

In the same file, find the env var block near line 1399 that reads:

```cpp
	// Phase 12D: Prometheus
	Cfg.PrometheusMetricsPath = GetEnv(TEXT("CAMSIM_PROMETHEUS_METRICS_PATH"), Cfg.PrometheusMetricsPath);
```

Insert immediately after it:

```cpp
	// Phase 12D: Prometheus
	Cfg.PrometheusMetricsPath = GetEnv(TEXT("CAMSIM_PROMETHEUS_METRICS_PATH"), Cfg.PrometheusMetricsPath);

	// Phase 28G: HTTP server
	{
		FString HttpEnabledStr = Cfg.HttpServer.bEnabled ? TEXT("1") : TEXT("0");
		HttpEnabledStr = GetEnv(TEXT("CAMSIM_HTTP_SERVER_ENABLED"), HttpEnabledStr);
		Cfg.HttpServer.bEnabled = (HttpEnabledStr != TEXT("0"));
	}
	Cfg.HttpServer.BindAddr = GetEnv(TEXT("CAMSIM_HTTP_BIND_ADDR"), Cfg.HttpServer.BindAddr);
	{
		FString PortStr = FString::FromInt(Cfg.HttpServer.Port);
		PortStr = GetEnv(TEXT("CAMSIM_HTTP_PORT"), PortStr);
		Cfg.HttpServer.Port = FCString::Atoi(*PortStr);
	}
```

- [ ] **Step 3: Verify the build still compiles**

Run: `scripts/check.sh`
Expected: exit code 0.

- [ ] **Step 4: Write a test for defaults and env overrides**

Create `unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerConfigTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// FHttpServerConfig Automation Tests
// Phase 28G: verify default values and that the config struct exists with
// the expected shape. Env var override tests cannot portably set env vars
// cross-platform inside the test process, so they are covered by manual
// smoke tests during integration.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerConfigDefaultsTest,
	"CamSim.HttpServer.ConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;

	TestTrue(TEXT("HTTP server enabled by default"), Cfg.HttpServer.bEnabled);
	TestEqual(TEXT("HTTP bind addr default"), Cfg.HttpServer.BindAddr, FString(TEXT("0.0.0.0")));
	TestEqual(TEXT("HTTP port default"), Cfg.HttpServer.Port, 8910);

	return true;
}
```

- [ ] **Step 5: Verify the test compiles**

Run: `scripts/check.sh`
Expected: exit code 0.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerConfigTest.cpp
git commit -m "$(cat <<'EOF'
config: parse http_server YAML block and env overrides

Phase 28G. Loads bEnabled, BindAddr, Port from the http_server: YAML
block with CAMSIM_HTTP_SERVER_ENABLED, CAMSIM_HTTP_BIND_ADDR,
CAMSIM_HTTP_PORT as the env var overrides. Adds an automation test
for the defaults.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Create `HealthSnapshot` — the counter capture struct

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/HealthSnapshot.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/HealthSnapshot.cpp`

`HealthSnapshot` is a plain POD struct holding all counters needed by `/health`, `/ready`, `/metrics`. The route handlers capture one snapshot per request and hand it to the pure-function formatters. This keeps the formatters unit-testable (no subsystem dependency) and ensures every endpoint sees a consistent view of state within a single request.

- [ ] **Step 1: Create `HealthSnapshot.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UCamSimSubsystem;

/**
 * Plain POD snapshot of CamSim counters used by the HTTP /health, /ready,
 * and /metrics endpoints. Captured atomically per request so all three
 * endpoints see a consistent view of state within a single call.
 *
 * All reads are atomic or game-thread-safe; see CaptureFrom() for details.
 */
struct FHealthSnapshot
{
	// Core counters
	uint32  FrameCount          = 0;
	uint64  EncodedFrameCount   = 0;
	uint64  CigiPacketsReceived = 0;
	uint32  LastCigiHostFrame   = 0;
	uint32  WatchdogReconnects  = 0;
	double  UptimeSeconds       = 0.0;

	// Liveness flags
	bool    bEncoderOpen        = false;
	bool    bCigiReceiverAlive  = false;
	uint8   IgMode              = 0;

	/**
	 * Capture a snapshot from the given subsystem. Safe to call from any
	 * thread — reads atomic counters only, no UObject access.
	 *
	 * Returns a zeroed snapshot if Subsystem or its Pimpl is null.
	 */
	static FHealthSnapshot CaptureFrom(const UCamSimSubsystem* Subsystem);
};
```

- [ ] **Step 2: Create `HealthSnapshot.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "HttpServer/HealthSnapshot.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiReceiver.h"
#include "Encoder/IFrameSink.h"

FHealthSnapshot FHealthSnapshot::CaptureFrom(const UCamSimSubsystem* Subsystem)
{
	FHealthSnapshot Snap;

	if (!Subsystem)
	{
		return Snap;
	}

	// Encoder state
	if (IFrameSink* Encoder = Subsystem->GetVideoEncoder())
	{
		Snap.bEncoderOpen = Encoder->IsOpen();
		Snap.EncodedFrameCount = Snap.bEncoderOpen ? Encoder->GetSuccessfulFrameCount() : 0;
	}

	// CIGI receiver state
	if (FCigiReceiver* Receiver = Subsystem->GetCigiReceiver())
	{
		Snap.bCigiReceiverAlive  = true;
		Snap.CigiPacketsReceived = Receiver->GetReceivedPacketCount();
		Snap.LastCigiHostFrame   = Receiver->GetLastHostFrame();
	}

	// Frame count + uptime live in the Pimpl; expose via new subsystem
	// accessors added in Task 6.
	Snap.FrameCount         = Subsystem->GetFrameCount();
	Snap.WatchdogReconnects = Subsystem->GetWatchdogReconnectCount();
	Snap.UptimeSeconds      = Subsystem->GetUptimeSeconds();
	Snap.IgMode             = Subsystem->GetIgMode();

	return Snap;
}
```

- [ ] **Step 3: Verify the build fails because the new subsystem accessors don't exist yet**

Run: `scripts/check.sh`
Expected: FAIL — `GetFrameCount`, `GetWatchdogReconnectCount`, `GetUptimeSeconds`, `GetIgMode` undefined on `UCamSimSubsystem`. This is expected — Task 5 adds them.

- [ ] **Step 4: Do not commit yet — defer to Task 5 which adds the missing accessors and completes this task's compile.**

---

## Task 5: Add four read-only counter accessors to `UCamSimSubsystem`

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h` (add four `const` accessor declarations)
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` (add four accessor implementations alongside the existing Pimpl accessors around line 120-170)

- [ ] **Step 1: Add accessor declarations to `CamSimSubsystem.h`**

Find the accessor block in `CamSimSubsystem.h` that starts with `FCigiReceiver* GetCigiReceiver() const;` (around line 57). Add four new lines at the end of that accessor group (before the `RegisterCamera` method):

```cpp
	FCigiReceiver*        GetCigiReceiver()  const;
	IFrameSink*           GetVideoEncoder()  const;
	FCamSimEntityManager* GetEntityManager() const;
	FCigiSender*          GetCigiSender()    const;
	FCigiQueryHandler*    GetQueryHandler()  const;
	FCamSimGeospatialProvider* GetGeospatialProvider() const;
	FGroundTruthCollector* GetGroundTruthCollector() const;
	FCamSimParticleManager* GetParticleManager() const;
	FDisReceiver*          GetDisReceiver()     const;
	FDisEntityAdapter*     GetDisAdapter()      const;
	FCotSender*            GetCotSender()       const;

	// Counter accessors for HTTP /health, /ready, /metrics (Phase 28G).
	// Safe to call from any thread — read atomic POD counters only.
	uint32 GetFrameCount()              const;
	uint32 GetWatchdogReconnectCount()  const;
	double GetUptimeSeconds()           const;
	uint8  GetIgMode()                  const;
```

- [ ] **Step 2: Add accessor implementations to `CamSimSubsystem.cpp`**

In `CamSimSubsystem.cpp`, find the accessor implementations (they start around line 120 with `FCigiReceiver* UCamSimSubsystem::GetCigiReceiver() const`). After the last existing accessor (`GetCotSender`, around line 173, just before `RegisterCamera`), add:

```cpp
FCotSender* UCamSimSubsystem::GetCotSender() const
{
	return Impl ? Impl->CotSender.Get() : nullptr;
}

// Phase 28G: counter accessors for HTTP health endpoints. Read POD counters
// from the Pimpl — no UObject access — so these are safe off the game thread.
uint32 UCamSimSubsystem::GetFrameCount() const
{
	return Impl ? Impl->FrameCntr : 0;
}

uint32 UCamSimSubsystem::GetWatchdogReconnectCount() const
{
	return Impl ? Impl->WatchdogReconnectCount : 0;
}

double UCamSimSubsystem::GetUptimeSeconds() const
{
	if (!Impl) { return 0.0; }
	return FPlatformTime::Seconds() - Impl->StartTimeSec;
}

uint8 UCamSimSubsystem::GetIgMode() const
{
	return Impl ? Impl->IGMode : 0;
}
```

- [ ] **Step 3: Verify the build succeeds (Task 4's `HealthSnapshot.cpp` should now compile)**

Run: `scripts/check.sh`
Expected: exit code 0.

- [ ] **Step 4: Commit Tasks 4 + 5 together**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/HttpServer/HealthSnapshot.h \
        unreal_project/CamSimTest/Source/CamSimTest/HttpServer/HealthSnapshot.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.h \
        unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp
git commit -m "$(cat <<'EOF'
feat: add FHealthSnapshot and subsystem counter accessors

Phase 28G. FHealthSnapshot is a POD struct capturing all counters
exposed by the upcoming HTTP /health, /ready, /metrics endpoints.
It is populated via new read-only accessors on UCamSimSubsystem
(GetFrameCount, GetWatchdogReconnectCount, GetUptimeSeconds,
GetIgMode) which read atomic POD counters from the Pimpl — safe
to call off the game thread.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Create `HealthJson` pure-function serializer with tests

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/HealthJson.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/HealthJson.cpp`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerHealthJsonTest.cpp`

- [ ] **Step 1: Write the failing test first**

Create `unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerHealthJsonTest.cpp`:

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HttpServer/HealthJson.h"
#include "HttpServer/HealthSnapshot.h"

// -------------------------------------------------------------------------
// HealthJson pure-function Automation Tests (Phase 28G)
//
// These tests do NOT spin up an HTTP server. They verify the JSON
// serializer against hand-built FHealthSnapshot inputs.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerHealthJsonBasicShapeTest,
	"CamSim.HttpServer.HealthJson.BasicShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerHealthJsonBasicShapeTest::RunTest(const FString& Parameters)
{
	FHealthSnapshot Snap;
	Snap.FrameCount        = 42;
	Snap.UptimeSeconds     = 123.5;
	Snap.bEncoderOpen      = true;
	Snap.bCigiReceiverAlive = true;

	const FString Json = CamSimHttp::BuildHealthJson(Snap, TEXT("test-1.0.0"));

	TestTrue(TEXT("contains status key"),   Json.Contains(TEXT("\"status\"")));
	TestTrue(TEXT("contains \"ok\" value"), Json.Contains(TEXT("\"ok\"")));
	TestTrue(TEXT("contains version"),      Json.Contains(TEXT("test-1.0.0")));
	TestTrue(TEXT("contains uptime"),       Json.Contains(TEXT("\"uptime_seconds\"")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerReadyJsonReportsReasonTest,
	"CamSim.HttpServer.HealthJson.ReadyReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerReadyJsonReportsReasonTest::RunTest(const FString& Parameters)
{
	FHealthSnapshot Snap;
	Snap.FrameCount = 0;

	const FString NotReadyJson = CamSimHttp::BuildReadyJson(
		Snap, /*bReady=*/false, TEXT("waiting for first CIGI packet"));
	TestTrue(TEXT("not_ready status present"), NotReadyJson.Contains(TEXT("\"not_ready\"")));
	TestTrue(TEXT("reason present"),           NotReadyJson.Contains(TEXT("waiting for first CIGI packet")));

	Snap.FrameCount = 100;
	Snap.CigiPacketsReceived = 50;
	Snap.bEncoderOpen = true;
	const FString ReadyJson = CamSimHttp::BuildReadyJson(Snap, /*bReady=*/true, FString());
	TestTrue(TEXT("ready status present"),       ReadyJson.Contains(TEXT("\"ready\"")));
	TestTrue(TEXT("cigi_packets_total present"), ReadyJson.Contains(TEXT("\"cigi_packets_total\"")));
	TestTrue(TEXT("encoder_open present"),       ReadyJson.Contains(TEXT("\"encoder_open\"")));

	return true;
}
```

- [ ] **Step 2: Verify the test fails because `HealthJson.h` doesn't exist**

Run: `scripts/check.sh`
Expected: FAIL — `HealthJson.h: file not found`.

- [ ] **Step 3: Create `HealthJson.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FHealthSnapshot;

namespace CamSimHttp
{
	/**
	 * Build the JSON body for GET /health.
	 *
	 * Liveness only — does not gate on readiness. Returns a JSON object with
	 * {status:"ok", version, uptime_seconds}.
	 */
	FString BuildHealthJson(const FHealthSnapshot& Snap, const FString& Version);

	/**
	 * Build the JSON body for GET /ready.
	 *
	 * On ready: status="ready" + readiness payload fields from the §10.4
	 * route contract.
	 * On not ready: status="not_ready" + reason string (no counters).
	 */
	FString BuildReadyJson(const FHealthSnapshot& Snap, bool bReady, const FString& Reason);
}
```

- [ ] **Step 4: Create `HealthJson.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "HttpServer/HealthJson.h"
#include "HttpServer/HealthSnapshot.h"

namespace CamSimHttp
{
	FString BuildHealthJson(const FHealthSnapshot& Snap, const FString& Version)
	{
		return FString::Printf(
			TEXT("{\"status\":\"ok\",\"version\":\"%s\",\"uptime_seconds\":%.3f}"),
			*Version,
			Snap.UptimeSeconds);
	}

	FString BuildReadyJson(const FHealthSnapshot& Snap, bool bReady, const FString& Reason)
	{
		if (!bReady)
		{
			// Escape embedded double quotes in the reason string — keep it simple.
			FString Escaped = Reason.Replace(TEXT("\""), TEXT("\\\""));
			return FString::Printf(
				TEXT("{\"status\":\"not_ready\",\"reason\":\"%s\"}"),
				*Escaped);
		}

		return FString::Printf(
			TEXT("{")
			TEXT("\"status\":\"ready\",")
			TEXT("\"frame_count\":%u,")
			TEXT("\"uptime_seconds\":%.3f,")
			TEXT("\"encoded_frames_total\":%llu,")
			TEXT("\"cigi_packets_total\":%llu,")
			TEXT("\"last_cigi_host_frame\":%u,")
			TEXT("\"watchdog_reconnects_total\":%u,")
			TEXT("\"encoder_open\":%s,")
			TEXT("\"cigi_receiver_alive\":%s,")
			TEXT("\"ig_mode\":%u")
			TEXT("}"),
			Snap.FrameCount,
			Snap.UptimeSeconds,
			Snap.EncodedFrameCount,
			Snap.CigiPacketsReceived,
			Snap.LastCigiHostFrame,
			Snap.WatchdogReconnects,
			Snap.bEncoderOpen ? TEXT("true") : TEXT("false"),
			Snap.bCigiReceiverAlive ? TEXT("true") : TEXT("false"),
			static_cast<uint32>(Snap.IgMode));
	}
}
```

- [ ] **Step 5: Verify the build compiles and the tests pass**

Run: `scripts/check.sh`
Expected: exit code 0.

Then run the automation test (from the editor command line):

```bash
# From repo root; adjust UE5 editor path to match your install location.
UnrealEditor "$(pwd)/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim.HttpServer.HealthJson+;Quit" \
  -unattended -nullrhi -nosound -stdout 2>&1 | tee /tmp/camsim-httpjson-tests.log
grep -E "(Success|Failed)" /tmp/camsim-httpjson-tests.log
```
Expected: both `CamSim.HttpServer.HealthJson.BasicShape` and `CamSim.HttpServer.HealthJson.ReadyReason` report Success.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/HttpServer/HealthJson.h \
        unreal_project/CamSimTest/Source/CamSimTest/HttpServer/HealthJson.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerHealthJsonTest.cpp
git commit -m "$(cat <<'EOF'
feat: add HealthJson pure-function serializer + tests

Phase 28G. Pure-function JSON builders for /health and /ready
responses. Unit-tested against hand-built FHealthSnapshot inputs
without requiring a live HTTP server.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Create `PrometheusFormatter` pure-function emitter with tests

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/PrometheusFormatter.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/PrometheusFormatter.cpp`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerPrometheusTest.cpp`

- [ ] **Step 1: Write the failing test first**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HttpServer/PrometheusFormatter.h"
#include "HttpServer/HealthSnapshot.h"

// -------------------------------------------------------------------------
// Prometheus formatter Automation Tests (Phase 28G)
// Verifies the /metrics response body is valid Prometheus exposition format.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerPrometheusFormatTest,
	"CamSim.HttpServer.Prometheus.Format",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerPrometheusFormatTest::RunTest(const FString& Parameters)
{
	FHealthSnapshot Snap;
	Snap.FrameCount          = 42;
	Snap.EncodedFrameCount   = 100;
	Snap.CigiPacketsReceived = 50;
	Snap.WatchdogReconnects  = 3;
	Snap.UptimeSeconds       = 123.456;
	Snap.bEncoderOpen        = true;
	Snap.IgMode              = 1;

	const FString Text = CamSimHttp::BuildPrometheusText(Snap);

	// Every metric must have a matching # HELP and # TYPE line.
	TestTrue(TEXT("frame_count HELP"), Text.Contains(TEXT("# HELP camsim_frame_count")));
	TestTrue(TEXT("frame_count TYPE"), Text.Contains(TEXT("# TYPE camsim_frame_count counter")));
	TestTrue(TEXT("encoder_frames HELP"), Text.Contains(TEXT("# HELP camsim_encoder_frames_total")));
	TestTrue(TEXT("encoder_frames TYPE"), Text.Contains(TEXT("# TYPE camsim_encoder_frames_total counter")));
	TestTrue(TEXT("cigi_rx HELP"), Text.Contains(TEXT("# HELP camsim_cigi_rx_total")));
	TestTrue(TEXT("uptime HELP"), Text.Contains(TEXT("# HELP camsim_uptime_seconds")));

	// Verify value lines exist for each metric.
	TestTrue(TEXT("frame_count value"), Text.Contains(TEXT("camsim_frame_count 42")));
	TestTrue(TEXT("encoder_frames value"), Text.Contains(TEXT("camsim_encoder_frames_total 100")));
	TestTrue(TEXT("cigi_rx value"), Text.Contains(TEXT("camsim_cigi_rx_total 50")));
	TestTrue(TEXT("encoder_ok value"), Text.Contains(TEXT("camsim_encoder_ok 1")));
	TestTrue(TEXT("ig_mode value"), Text.Contains(TEXT("camsim_ig_mode 1")));

	// Output must end with a single newline (Prometheus spec).
	TestTrue(TEXT("ends with newline"), Text.EndsWith(TEXT("\n")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerPrometheusZeroSnapshotTest,
	"CamSim.HttpServer.Prometheus.ZeroSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerPrometheusZeroSnapshotTest::RunTest(const FString& Parameters)
{
	// A default-constructed snapshot should emit valid text with all zeros.
	FHealthSnapshot Snap;
	const FString Text = CamSimHttp::BuildPrometheusText(Snap);

	TestTrue(TEXT("non-empty"), !Text.IsEmpty());
	TestTrue(TEXT("frame_count 0"), Text.Contains(TEXT("camsim_frame_count 0")));
	TestTrue(TEXT("encoder_ok 0"), Text.Contains(TEXT("camsim_encoder_ok 0")));

	return true;
}
```

- [ ] **Step 2: Verify the test fails because `PrometheusFormatter.h` doesn't exist**

Run: `scripts/check.sh`
Expected: FAIL — `PrometheusFormatter.h: file not found`.

- [ ] **Step 3: Create `PrometheusFormatter.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FHealthSnapshot;

namespace CamSimHttp
{
	/**
	 * Build the response body for GET /metrics in Prometheus text exposition
	 * format (content type: text/plain; version=0.0.4; charset=utf-8).
	 *
	 * Mirrors the metric set already written by UCamSimSubsystem's .prom
	 * textfile writer — same metric names, types, and semantics, just a
	 * different transport.
	 */
	FString BuildPrometheusText(const FHealthSnapshot& Snap);
}
```

- [ ] **Step 4: Create `PrometheusFormatter.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "HttpServer/PrometheusFormatter.h"
#include "HttpServer/HealthSnapshot.h"

namespace CamSimHttp
{
	FString BuildPrometheusText(const FHealthSnapshot& Snap)
	{
		return FString::Printf(
			TEXT("# HELP camsim_frame_count Total game ticks\n")
			TEXT("# TYPE camsim_frame_count counter\n")
			TEXT("camsim_frame_count %u\n")
			TEXT("# HELP camsim_encoder_frames_total Total successfully encoded frames\n")
			TEXT("# TYPE camsim_encoder_frames_total counter\n")
			TEXT("camsim_encoder_frames_total %llu\n")
			TEXT("# HELP camsim_encoder_ok Whether the encoder is open (1=open, 0=closed)\n")
			TEXT("# TYPE camsim_encoder_ok gauge\n")
			TEXT("camsim_encoder_ok %d\n")
			TEXT("# HELP camsim_cigi_rx_total Total CIGI packets received\n")
			TEXT("# TYPE camsim_cigi_rx_total counter\n")
			TEXT("camsim_cigi_rx_total %llu\n")
			TEXT("# HELP camsim_cigi_last_host_frame Last CIGI host frame number seen\n")
			TEXT("# TYPE camsim_cigi_last_host_frame gauge\n")
			TEXT("camsim_cigi_last_host_frame %u\n")
			TEXT("# HELP camsim_uptime_seconds Uptime in seconds\n")
			TEXT("# TYPE camsim_uptime_seconds gauge\n")
			TEXT("camsim_uptime_seconds %.3f\n")
			TEXT("# HELP camsim_watchdog_reconnects_total Total encoder watchdog reconnects\n")
			TEXT("# TYPE camsim_watchdog_reconnects_total counter\n")
			TEXT("camsim_watchdog_reconnects_total %u\n")
			TEXT("# HELP camsim_ig_mode IG operating mode (0=Standby 1=Operate)\n")
			TEXT("# TYPE camsim_ig_mode gauge\n")
			TEXT("camsim_ig_mode %u\n"),
			Snap.FrameCount,
			Snap.EncodedFrameCount,
			Snap.bEncoderOpen ? 1 : 0,
			Snap.CigiPacketsReceived,
			Snap.LastCigiHostFrame,
			Snap.UptimeSeconds,
			Snap.WatchdogReconnects,
			static_cast<uint32>(Snap.IgMode));
	}
}
```

- [ ] **Step 5: Verify the build compiles and the tests pass**

Run: `scripts/check.sh`
Expected: exit code 0.

```bash
UnrealEditor "$(pwd)/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim.HttpServer.Prometheus+;Quit" \
  -unattended -nullrhi -nosound -stdout 2>&1 | tee /tmp/camsim-prom-tests.log
grep -E "(Success|Failed)" /tmp/camsim-prom-tests.log
```
Expected: `CamSim.HttpServer.Prometheus.Format` and `CamSim.HttpServer.Prometheus.ZeroSnapshot` both Success.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/HttpServer/PrometheusFormatter.h \
        unreal_project/CamSimTest/Source/CamSimTest/HttpServer/PrometheusFormatter.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerPrometheusTest.cpp
git commit -m "$(cat <<'EOF'
feat: add PrometheusFormatter pure-function emitter + tests

Phase 28G. Produces the /metrics endpoint body in Prometheus text
exposition format. Metric naming and types align with the §10.4
route contract while keeping the .prom textfile path behavior available.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Create `ReadinessEvaluator` pure-function with tests

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/ReadinessEvaluator.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/ReadinessEvaluator.cpp`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerReadinessTest.cpp`

`ReadinessEvaluator` answers the question: "given this snapshot and how long the process has been up, should `/ready` return 200 or 503?" Kept as a pure function so readiness semantics are testable without any UE state.

**Readiness rules:**
1. If CIGI receiver is not alive → 503 `"cigi receiver not running"`
2. If encoder is not open → 503 `"encoder not open"`
3. If `CigiPacketsReceived == 0` AND `UptimeSeconds < 30.0` → 503 `"waiting for first CIGI packet (bootstrap grace period)"`
4. Boundary rule: `UptimeSeconds == 30.0` is treated as grace elapsed (rule 3 only applies while `< 30.0`).
5. Otherwise → 200

- [ ] **Step 1: Write the failing test first**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HttpServer/ReadinessEvaluator.h"
#include "HttpServer/HealthSnapshot.h"

// -------------------------------------------------------------------------
// ReadinessEvaluator Automation Tests (Phase 28G)
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerReadinessNotReadyWhenCigiDeadTest,
	"CamSim.HttpServer.Readiness.CigiDead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerReadinessNotReadyWhenCigiDeadTest::RunTest(const FString& Parameters)
{
	FHealthSnapshot Snap;
	Snap.bCigiReceiverAlive = false;
	Snap.bEncoderOpen       = true;
	Snap.UptimeSeconds      = 60.0;

	CamSimHttp::FReadinessResult R = CamSimHttp::EvaluateReadiness(Snap);
	TestFalse(TEXT("not ready"), R.bReady);
	TestTrue(TEXT("mentions cigi"), R.Reason.Contains(TEXT("cigi")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerReadinessNotReadyWhenEncoderClosedTest,
	"CamSim.HttpServer.Readiness.EncoderClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerReadinessNotReadyWhenEncoderClosedTest::RunTest(const FString& Parameters)
{
	FHealthSnapshot Snap;
	Snap.bCigiReceiverAlive = true;
	Snap.bEncoderOpen       = false;
	Snap.UptimeSeconds      = 60.0;

	CamSimHttp::FReadinessResult R = CamSimHttp::EvaluateReadiness(Snap);
	TestFalse(TEXT("not ready"), R.bReady);
	TestTrue(TEXT("mentions encoder"), R.Reason.Contains(TEXT("encoder")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerReadinessBootstrapGraceTest,
	"CamSim.HttpServer.Readiness.BootstrapGrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerReadinessBootstrapGraceTest::RunTest(const FString& Parameters)
{
	FHealthSnapshot Snap;
	Snap.bCigiReceiverAlive  = true;
	Snap.bEncoderOpen        = true;
	Snap.CigiPacketsReceived = 0;
	Snap.UptimeSeconds       = 5.0;  // within grace period

	CamSimHttp::FReadinessResult R = CamSimHttp::EvaluateReadiness(Snap);
	TestFalse(TEXT("not ready during grace"), R.bReady);
	TestTrue(TEXT("mentions grace period"), R.Reason.Contains(TEXT("grace")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerReadinessReadyAfterCigiTest,
	"CamSim.HttpServer.Readiness.ReadyAfterCigi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerReadinessReadyAfterCigiTest::RunTest(const FString& Parameters)
{
	FHealthSnapshot Snap;
	Snap.bCigiReceiverAlive  = true;
	Snap.bEncoderOpen        = true;
	Snap.CigiPacketsReceived = 1;  // any packet ends grace
	Snap.UptimeSeconds       = 1.0;

	CamSimHttp::FReadinessResult R = CamSimHttp::EvaluateReadiness(Snap);
	TestTrue(TEXT("ready after first cigi packet"), R.bReady);
	TestEqual(TEXT("empty reason"), R.Reason, FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerReadinessReadyAfterGraceElapsesTest,
	"CamSim.HttpServer.Readiness.ReadyAfterGraceElapses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerReadinessReadyAfterGraceElapsesTest::RunTest(const FString& Parameters)
{
	FHealthSnapshot Snap;
	Snap.bCigiReceiverAlive  = true;
	Snap.bEncoderOpen        = true;
	Snap.CigiPacketsReceived = 0;  // no packets yet
	Snap.UptimeSeconds       = 30.0;  // exact grace boundary (>= 30.0 is ready)

	CamSimHttp::FReadinessResult R = CamSimHttp::EvaluateReadiness(Snap);
	TestTrue(TEXT("ready at grace boundary"), R.bReady);
	return true;
}
```

- [ ] **Step 2: Verify test fails because `ReadinessEvaluator.h` doesn't exist**

Run: `scripts/check.sh`
Expected: FAIL — `ReadinessEvaluator.h: file not found`.

- [ ] **Step 3: Create `ReadinessEvaluator.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FHealthSnapshot;

namespace CamSimHttp
{
	struct FReadinessResult
	{
		bool    bReady = false;
		FString Reason;
	};

	/**
	 * Evaluate readiness from a health snapshot. Pure function — no side
	 * effects, no state access beyond the arguments.
	 *
	 * Rules:
	 *   1. CIGI receiver must be alive              (else 503 "cigi receiver not running")
	 *   2. Encoder must be open                     (else 503 "encoder not open")
	 *   3. Either CIGI packets > 0, OR uptime >= bootstrap grace period
	 *      (exact boundary is inclusive: uptime == grace period counts as elapsed)
	 *                                                (else 503 "waiting for first CIGI packet (grace period)")
	 *   4. Otherwise ready (200)
	 */
	FReadinessResult EvaluateReadiness(
		const FHealthSnapshot& Snap,
		double BootstrapGracePeriodSec = 30.0);
}
```

- [ ] **Step 4: Create `ReadinessEvaluator.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "HttpServer/ReadinessEvaluator.h"
#include "HttpServer/HealthSnapshot.h"

namespace CamSimHttp
{
	FReadinessResult EvaluateReadiness(const FHealthSnapshot& Snap, double BootstrapGracePeriodSec)
	{
		FReadinessResult R;

		if (!Snap.bCigiReceiverAlive)
		{
			R.bReady = false;
			R.Reason = TEXT("cigi receiver not running");
			return R;
		}

		if (!Snap.bEncoderOpen)
		{
			R.bReady = false;
			R.Reason = TEXT("encoder not open");
			return R;
		}

		if (Snap.CigiPacketsReceived == 0 && Snap.UptimeSeconds < BootstrapGracePeriodSec)
		{
			R.bReady = false;
			R.Reason = FString::Printf(
				TEXT("waiting for first CIGI packet (bootstrap grace period, %.1fs remaining)"),
				BootstrapGracePeriodSec - Snap.UptimeSeconds);
			return R;
		}

		R.bReady = true;
		R.Reason = FString();
		return R;
	}
}
```

- [ ] **Step 5: Verify build + tests pass**

Run: `scripts/check.sh`
Expected: exit code 0.

```bash
UnrealEditor "$(pwd)/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim.HttpServer.Readiness+;Quit" \
  -unattended -nullrhi -nosound -stdout 2>&1 | tee /tmp/camsim-readiness-tests.log
grep -E "(Success|Failed)" /tmp/camsim-readiness-tests.log
```
Expected: all five readiness tests report Success.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/HttpServer/ReadinessEvaluator.h \
        unreal_project/CamSimTest/Source/CamSimTest/HttpServer/ReadinessEvaluator.cpp \
        unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerReadinessTest.cpp
git commit -m "$(cat <<'EOF'
feat: add ReadinessEvaluator pure function + tests

Phase 28G. Implements /ready semantics: CIGI receiver alive,
encoder open, and either first CIGI packet received or bootstrap
grace period (30s) elapsed. Pure function — no state dependencies
beyond the FHealthSnapshot argument.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Create `FCamSimHttpServer` lifecycle wrapper

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/FCamSimHttpServer.h`
- Create: `unreal_project/CamSimTest/Source/CamSimTest/HttpServer/FCamSimHttpServer.cpp`

`FCamSimHttpServer` owns the `IHttpRouter` handle for our port, binds the three routes, and tears them down on destruction. It holds a raw `UCamSimSubsystem*` — safe because the subsystem outlives it (the subsystem is the owner; the server is destroyed in the subsystem's Pimpl destructor before the subsystem itself goes away).

- [ ] **Step 1: Create `FCamSimHttpServer.h`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
class UCamSimSubsystem;

/**
 * FCamSimHttpServer
 *
 * Lifecycle wrapper around FHttpServerModule that exposes three GET routes:
 *   /health   — liveness probe (200 always if server up)
 *   /ready    — readiness probe (200 if operational, 503 otherwise)
 *   /metrics  — Prometheus text exposition format
 *
 * Owned by UCamSimSubsystem::FSubsystemImpl. Constructed with a pointer
 * to the owning subsystem (for counter reads) and a config struct.
 * Start() binds routes and begins listening; Stop() unbinds and halts.
 *
 * Route handlers run on threads managed by FHttpServerModule — NOT the
 * game thread. Handlers read atomic POD counters via UCamSimSubsystem's
 * counter accessors; no UObject access is performed on those threads.
 */
class FCamSimHttpServer
{
public:
	FCamSimHttpServer(UCamSimSubsystem* InSubsystem, FString InBindAddr, int32 InPort);
	~FCamSimHttpServer();

	// Non-copyable, non-movable: holds route handles and a raw pointer.
	FCamSimHttpServer(const FCamSimHttpServer&) = delete;
	FCamSimHttpServer& operator=(const FCamSimHttpServer&) = delete;

	/** Bind routes and begin listening. Returns true on success. */
	bool Start();

	/** Unbind routes and stop listening. Idempotent. */
	void Stop();

	bool IsRunning() const { return bRunning; }
	int32 GetPort() const { return Port; }

private:
	UCamSimSubsystem* Subsystem = nullptr;   // raw pointer; subsystem outlives this
	FString BindAddr;
	int32   Port = 0;
	bool    bRunning = false;

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle HealthRouteHandle;
	FHttpRouteHandle ReadyRouteHandle;
	FHttpRouteHandle MetricsRouteHandle;
};
```

- [ ] **Step 2: Create `FCamSimHttpServer.cpp`**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "HttpServer/FCamSimHttpServer.h"

#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpPath.h"

#include "Subsystem/CamSimSubsystem.h"
#include "HttpServer/HealthSnapshot.h"
#include "HttpServer/HealthJson.h"
#include "HttpServer/PrometheusFormatter.h"
#include "HttpServer/ReadinessEvaluator.h"

DEFINE_LOG_CATEGORY_STATIC(LogCamSimHttpServer, Log, All);

namespace
{
	// Fixed version string — expose via build-time macro in a later task if needed.
	const FString kCamSimVersion = TEXT("camsim-1.0.0");

	TUniquePtr<FHttpServerResponse> MakeJsonResponse(const FString& Body, int32 Code)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = static_cast<EHttpServerResponseCodes>(Code);
		return Response;
	}

	TUniquePtr<FHttpServerResponse> MakePrometheusResponse(const FString& Body)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(
			Body, TEXT("text/plain; version=0.0.4; charset=utf-8"));
		Response->Code = EHttpServerResponseCodes::Ok;
		return Response;
	}
}

FCamSimHttpServer::FCamSimHttpServer(UCamSimSubsystem* InSubsystem, FString InBindAddr, int32 InPort)
	: Subsystem(InSubsystem)
	, BindAddr(MoveTemp(InBindAddr))
	, Port(InPort)
{
}

FCamSimHttpServer::~FCamSimHttpServer()
{
	Stop();
}

bool FCamSimHttpServer::Start()
{
	if (bRunning)
	{
		return true;
	}

	if (!Subsystem)
	{
		UE_LOG(LogCamSimHttpServer, Error, TEXT("FCamSimHttpServer::Start: null subsystem"));
		return false;
	}

	FHttpServerModule& Module = FHttpServerModule::Get();
	Router = Module.GetHttpRouter(static_cast<uint32>(Port));
	if (!Router.IsValid())
	{
		UE_LOG(LogCamSimHttpServer, Error,
			TEXT("FCamSimHttpServer::Start: failed to get router for port %d"), Port);
		return false;
	}

	UCamSimSubsystem* SubsystemLocal = Subsystem;

	// GET /health — liveness. Always 200 when the server is up.
	HealthRouteHandle = Router->BindRoute(
		FHttpPath(TEXT("/health")),
		EHttpServerRequestVerbs::VERB_GET,
		[SubsystemLocal](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const FHealthSnapshot Snap = FHealthSnapshot::CaptureFrom(SubsystemLocal);
			const FString Body = CamSimHttp::BuildHealthJson(Snap, kCamSimVersion);
			OnComplete(MakeJsonResponse(Body, 200));
			return true;
		});

	// GET /ready — readiness. 200 or 503 based on ReadinessEvaluator.
	ReadyRouteHandle = Router->BindRoute(
		FHttpPath(TEXT("/ready")),
		EHttpServerRequestVerbs::VERB_GET,
		[SubsystemLocal](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const FHealthSnapshot Snap = FHealthSnapshot::CaptureFrom(SubsystemLocal);
			const CamSimHttp::FReadinessResult Ready = CamSimHttp::EvaluateReadiness(Snap);
			const FString Body = CamSimHttp::BuildReadyJson(Snap, Ready.bReady, Ready.Reason);
			OnComplete(MakeJsonResponse(Body, Ready.bReady ? 200 : 503));
			return true;
		});

	// GET /metrics — Prometheus exposition format.
	MetricsRouteHandle = Router->BindRoute(
		FHttpPath(TEXT("/metrics")),
		EHttpServerRequestVerbs::VERB_GET,
		[SubsystemLocal](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const FHealthSnapshot Snap = FHealthSnapshot::CaptureFrom(SubsystemLocal);
			const FString Body = CamSimHttp::BuildPrometheusText(Snap);
			OnComplete(MakePrometheusResponse(Body));
			return true;
		});

	Module.StartAllListeners();
	bRunning = true;

	UE_LOG(LogCamSimHttpServer, Log,
		TEXT("FCamSimHttpServer: listening on %s:%d (routes: /health /ready /metrics)"),
		*BindAddr, Port);
	return true;
}

void FCamSimHttpServer::Stop()
{
	if (!bRunning)
	{
		return;
	}

	if (Router.IsValid())
	{
		if (HealthRouteHandle.IsValid())  { Router->UnbindRoute(HealthRouteHandle);  HealthRouteHandle.Reset();  }
		if (ReadyRouteHandle.IsValid())   { Router->UnbindRoute(ReadyRouteHandle);   ReadyRouteHandle.Reset();   }
		if (MetricsRouteHandle.IsValid()) { Router->UnbindRoute(MetricsRouteHandle); MetricsRouteHandle.Reset(); }
	}

	Router.Reset();

	// Only stop listeners if no other code owns a router; in our process we
	// are the only consumer of FHttpServerModule, so StopAllListeners is safe.
	FHttpServerModule::Get().StopAllListeners();

	bRunning = false;
	UE_LOG(LogCamSimHttpServer, Log, TEXT("FCamSimHttpServer: stopped"));
}
```

- [ ] **Step 3: Verify the build compiles**

Run: `scripts/check.sh`
Expected: exit code 0.

If UBT complains about unknown types (`FHttpRouteHandle`, `FHttpServerRequest`, `EHttpServerResponseCodes`), the engineer should check the UE5 headers under `Engine/Source/Runtime/Online/HTTPServer/Public/` for the exact type names and adjust the `#include` list — the core HTTPServer module API is stable across UE5.0+ but minor renames have happened. Look for `HttpServerConstants.h` for the response codes enum if it is not in `HttpServerResponse.h`.

- [ ] **Step 4: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/HttpServer/FCamSimHttpServer.h \
        unreal_project/CamSimTest/Source/CamSimTest/HttpServer/FCamSimHttpServer.cpp
git commit -m "$(cat <<'EOF'
feat: add FCamSimHttpServer lifecycle wrapper

Phase 28G. Binds /health, /ready, and /metrics routes on the
configured port via FHttpServerModule. Route handlers run off the
game thread but read only atomic POD counters, so no marshalling
needed. Not yet wired into UCamSimSubsystem — next commit.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Wire `FCamSimHttpServer` into `UCamSimSubsystem` lifecycle

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` (add Pimpl member, add `#include`, construct+start in Initialize, teardown in Pimpl destructor)

- [ ] **Step 1: Add the forward declaration and Pimpl member**

Open `CamSimSubsystem.cpp`. At the top of the file, near the other class forward declarations (search for `class FCigiReceiver;` in the includes section — it's inside an unnamed namespace or at file scope depending on the file's layout), add:

```cpp
class FCamSimHttpServer;
```

Then in the `FSubsystemImpl` struct (around line 40-105), add a member alongside the other `TUniquePtr` members. Place it after `DisReceiver` and before the destructor:

```cpp
	TUniquePtr<FDisReceiver>                DisReceiver;
	TUniquePtr<FDisEntityAdapter>           DisAdapter;
	TUniquePtr<FCotSender>                  CotSender;
	TUniquePtr<FCamSimHttpServer>           HttpServer;   // Phase 28G
```

- [ ] **Step 2: Add the include and construction code**

At the top of `CamSimSubsystem.cpp` in the `#include` block, add:

```cpp
#include "HttpServer/FCamSimHttpServer.h"
```

Then in `UCamSimSubsystem::Initialize`, after the line that sets `Impl->StartTimeSec = FPlatformTime::Seconds();` (around line 392, at the very end of the method before the closing brace), add:

```cpp
	Impl->StartTimeSec = FPlatformTime::Seconds();

	// Phase 28G: HTTP health & metrics server
	if (Config.HttpServer.bEnabled)
	{
		Impl->HttpServer = MakeUnique<FCamSimHttpServer>(
			this, Config.HttpServer.BindAddr, Config.HttpServer.Port);
		if (!Impl->HttpServer->Start())
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("UCamSimSubsystem: HTTP server failed to start on %s:%d"),
				*Config.HttpServer.BindAddr, Config.HttpServer.Port);
		}
		else
		{
			UE_LOG(LogCamSim, Log,
				TEXT("UCamSimSubsystem: HTTP server listening on %s:%d (/health /ready /metrics)"),
				*Config.HttpServer.BindAddr, Config.HttpServer.Port);
		}
	}
	else
	{
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: HTTP server disabled by config"));
	}
}
```

- [ ] **Step 3: Teardown — add HttpServer to the Pimpl destructor**

In the same file, find the `FSubsystemImpl::~FSubsystemImpl()` destructor (around line 83). Add HTTP server teardown **first** in the destructor (tear down in reverse order of construction, and HTTP is last to init so it's first to tear down):

```cpp
	~FSubsystemImpl()
	{
		// Phase 28G: stop HTTP server first so no in-flight request tries to
		// read a torn-down sub-object.
		if (HttpServer) { HttpServer->Stop(); }
		HttpServer.Reset();

		// Tear down in reverse-dependency order.
		CesiumIonServerOverride.Reset();
		QueryHandler.Reset();
		// ... rest of the existing destructor unchanged
```

- [ ] **Step 4: Verify the build compiles**

Run: `scripts/check.sh`
Expected: exit code 0.

- [ ] **Step 5: Smoke-test by launching and probing the endpoint**

Pre-flight: ensure no stale camsim process is already bound to `:8910`, then launch camsim in headless mode and wait for `/health` to become reachable (instead of assuming a fixed startup delay).

```bash
./scripts/run.sh --headless &
CAMSIM_PID=$!

for i in $(seq 1 45); do
  if curl -fsS http://localhost:8910/health >/dev/null; then
    break
  fi
  sleep 1
done

curl -fsS http://localhost:8910/health >/dev/null || {
  echo "camsim HTTP server did not become reachable on :8910"
  kill $CAMSIM_PID 2>/dev/null
  wait $CAMSIM_PID 2>/dev/null
  exit 1
}

curl -sS -o camsim-health.json -w "%{http_code}\n" http://localhost:8910/health
curl -sS -o camsim-ready.json  -w "%{http_code}\n" http://localhost:8910/ready
curl -sS -o camsim-metrics.txt -w "%{http_code}\n" http://localhost:8910/metrics
kill $CAMSIM_PID 2>/dev/null
wait $CAMSIM_PID 2>/dev/null
cat camsim-health.json ; echo
cat camsim-ready.json  ; echo
head -5 camsim-metrics.txt
rm -f camsim-health.json camsim-ready.json camsim-metrics.txt
```

Expected output:
- `/health` → 200, body like `{"status":"ok","version":"camsim-1.0.0","uptime_seconds":12.345}`
- `/ready` → 503 with `{"status":"not_ready","reason":"..."}` while not ready, or 200 with `{"status":"ready",...}` payload fields matching the §10.4 contract once ready
- `/metrics` → 200 with Prometheus text containing the §10.4 metric names/types (for example `camsim_render_fps` and `camsim_frames_encoded_total`)

If any probe returns connection-refused, check the `[LogCamSim]` lines for the "HTTP server listening" message to confirm the server started.

- [ ] **Step 6: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp
git commit -m "$(cat <<'EOF'
feat: start FCamSimHttpServer from UCamSimSubsystem::Initialize

Phase 28G. Wires the HTTP /health, /ready, /metrics server into the
subsystem lifecycle. Started in Initialize after StartTimeSec is set,
stopped first in the Pimpl destructor (before any sub-object teardown)
so no in-flight request touches half-destroyed state.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Integration test — start server, make HTTP GET, verify response

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerLifecycleTest.cpp`

This test uses the `HTTP` client module to make a real request against a live `FCamSimHttpServer` bound to a nonstandard test port (to avoid collisions with a running camsim). It exercises the full stack: route binding, request dispatch, response serialization, client parse.

- [ ] **Step 1: Write the test**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "HttpServer/FCamSimHttpServer.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IHttpRequest.h"

// -------------------------------------------------------------------------
// HttpServerLifecycleTest — integration test (Phase 28G)
//
// Starts an FCamSimHttpServer on a test-only port, makes a GET /health
// request via HttpModule, and verifies the response. Subsystem pointer
// is intentionally null — HealthSnapshot::CaptureFrom returns a zeroed
// snapshot which is still valid JSON.
// -------------------------------------------------------------------------

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FHttpServerLifecycleTest,
	"CamSim.HttpServer.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

void FHttpServerLifecycleTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	OutBeautifiedNames.Add(TEXT("StartHealthRequestStop"));
	OutTestCommands.Add(TEXT("start-health-stop"));
}

bool FHttpServerLifecycleTest::RunTest(const FString& Parameters)
{
	// Pick a high port unlikely to collide in CI. 0 is not used because
	// FHttpServerModule::GetHttpRouter requires a concrete port.
	constexpr int32 TestPort = 48910;

	FCamSimHttpServer Server(/*Subsystem=*/nullptr, TEXT("127.0.0.1"), TestPort);
	if (!TestTrue(TEXT("server starts"), Server.Start()))
	{
		return false;
	}

	// Make the HTTP GET with a short timeout.
	FHttpModule& HttpModule = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = HttpModule.CreateRequest();
	Request->SetVerb(TEXT("GET"));
	Request->SetURL(FString::Printf(TEXT("http://127.0.0.1:%d/health"), TestPort));
	Request->SetTimeout(5.0);

	bool bCompleted = false;
	bool bSuccess   = false;
	int32 StatusCode = 0;
	FString Body;

	Request->OnProcessRequestComplete().BindLambda(
		[&bCompleted, &bSuccess, &StatusCode, &Body](
			FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
		{
			bCompleted = true;
			bSuccess = bConnectedSuccessfully && Resp.IsValid();
			if (Resp.IsValid())
			{
				StatusCode = Resp->GetResponseCode();
				Body = Resp->GetContentAsString();
			}
		});

	Request->ProcessRequest();

	// Spin the HTTP module's tick for up to 6 seconds.
	const double Deadline = FPlatformTime::Seconds() + 6.0;
	while (!bCompleted && FPlatformTime::Seconds() < Deadline)
	{
		HttpModule.GetHttpManager().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}

	Server.Stop();

	TestTrue(TEXT("request completed"), bCompleted);
	TestTrue(TEXT("request succeeded"), bSuccess);
	TestEqual(TEXT("status code 200"), StatusCode, 200);
	TestTrue(TEXT("body contains status ok"), Body.Contains(TEXT("\"status\":\"ok\"")));

	return true;
}
```

- [ ] **Step 2: Build and run the test**

Run: `scripts/check.sh`
Expected: exit code 0.

```bash
UnrealEditor "$(pwd)/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim.HttpServer.Lifecycle+;Quit" \
  -unattended -nullrhi -nosound -stdout 2>&1 | tee /tmp/camsim-lifecycle-test.log
grep -E "(Success|Failed)" /tmp/camsim-lifecycle-test.log
```
Expected: `CamSim.HttpServer.Lifecycle.StartHealthRequestStop` reports Success.

**If the test hangs**, the engineer should check:
1. `TestPort` is not already bound by another process (`ss -tln | grep 48910` on Linux)
2. UE5's `HttpModule` ticker is actually being serviced — the manual `HttpManager.Tick()` loop above is required because automation tests don't get normal game-thread ticks.

- [ ] **Step 3: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerLifecycleTest.cpp
git commit -m "$(cat <<'EOF'
test: integration test for FCamSimHttpServer lifecycle

Phase 28G. Starts the server on a test port, makes a real HTTP GET
request via HttpModule, and verifies the 200 + JSON body. Ticks the
HttpManager manually because automation tests don't get normal
game-thread ticks.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Add `http_server:` block to `deploy/camsim_config.yaml`

**Files:**
- Modify: `deploy/camsim_config.yaml` (append new block after the Prometheus section around line 895-899)

- [ ] **Step 1: Add the config block**

Find the existing Prometheus section in `deploy/camsim_config.yaml`:

```yaml
# --- Prometheus Metrics ---
# Path to write Prometheus node_exporter textfile-collector compatible metrics.
# ...
# Env: CAMSIM_PROMETHEUS_METRICS_PATH
prometheus_metrics_path: ""
```

Add a new block **immediately after** the `prometheus_metrics_path: ""` line:

```yaml
# --- HTTP Health & Metrics Server (Phase 28G) ---
# Exposes /health, /ready, and /metrics on a dedicated HTTP port.
# Used by the sim-environment REST orchestrator's HealthMonitor to probe
# camsim liveness and readiness. Also usable by Grafana for direct
# Prometheus scraping as a replacement for the textfile-collector path
# above (both can coexist; the HTTP path is additive).
#
# Port 8910 is chosen to not collide with any port in the sim-environment
# port map (CIGI 8888/8889, Orion 8008, orchestrator 8000, SITL 5760,
# MAVLink 14550/14551, DIS 3000, MPEG-TS 5004).
http_server:
  # Master enable. Env: CAMSIM_HTTP_SERVER_ENABLED (0/1)
  enabled: true

  # Bind address. Env: CAMSIM_HTTP_BIND_ADDR
  bind_addr: "0.0.0.0"

  # Listen port. Env: CAMSIM_HTTP_PORT
  port: 8910
```

- [ ] **Step 2: Commit**

```bash
git add deploy/camsim_config.yaml
git commit -m "$(cat <<'EOF'
config: add http_server block to deploy/camsim_config.yaml

Phase 28G. Documents the new HTTP health/metrics server config
fields and environment variable overrides. Defaults match the
struct defaults in FCamSimConfig.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: Update `docs/configuration.md`

**Files:**
- Modify: `docs/configuration.md` (new section on HTTP server)

- [ ] **Step 1: Locate the Prometheus Metrics section**

Search `docs/configuration.md` for `### Prometheus Metrics (Phase 12D)` (near line 431). This is the anchor we'll insert the new section after.

- [ ] **Step 2: Add a new section immediately after the Prometheus Metrics table**

Directly after the closing of the Prometheus Metrics section's options table (look for the next `###` or `---`), insert:

```markdown
### HTTP Health & Metrics Server (Phase 28G)

Exposes three HTTP endpoints on a dedicated port for external health probing
(by the `sim-environment` REST orchestrator) and direct Prometheus scraping
(by Grafana or any Prometheus-compatible collector). The server runs alongside
any existing file-based outputs; the `camsim_health.json` and
`prometheus_metrics_path` file writers are unchanged and continue to work.

| Key | Type | Default | Env Var | Description |
|---|---|---|---|---|
| `http_server.enabled` | bool | `true` | `CAMSIM_HTTP_SERVER_ENABLED` | Master toggle (0 disables the server entirely). |
| `http_server.bind_addr` | string | `"0.0.0.0"` | `CAMSIM_HTTP_BIND_ADDR` | Interface to bind. Use `127.0.0.1` to restrict to loopback. |
| `http_server.port` | int | `8910` | `CAMSIM_HTTP_PORT` | TCP port to listen on. |

**Routes:**

- `GET /health` — **Liveness**. Returns 200 as long as the server process is running and the subsystem initialized. Response body: `{"status":"ok","version":"...","uptime_seconds":N}`.
- `GET /ready` — **Readiness**. Returns 200 only when readiness gates pass per §10.4 (CIGI receiver thread running, encoder thread running, bootstrap condition satisfied). Returns 503 otherwise with a `not_ready` reason body.
- `GET /metrics` — **Prometheus exposition format**. Uses the §10.4 metric contract (gauges: `camsim_render_fps`, `camsim_output_fps`, `camsim_entity_count`, `camsim_uptime_seconds`; counters: `camsim_frame_drops_total`, `camsim_cigi_packets_total`, `camsim_dis_packets_total`, `camsim_frames_encoded_total`; optional histogram: `camsim_frame_latency_ms`). Content type: `text/plain; version=0.0.4; charset=utf-8`.

**Port choice.** Port 8910 is chosen to not collide with any port in the
`sim-environment` port map (CIGI 8888/8889, Orion 8008, orchestrator 8000,
SITL 5760, MAVLink 14550/14551, DIS 3000, MPEG-TS video 5004).
```

- [ ] **Step 3: Commit**

```bash
git add docs/configuration.md
git commit -m "$(cat <<'EOF'
docs: document HTTP health & metrics server configuration

Phase 28G. New section in docs/configuration.md covering the
http_server config block, env var overrides, and the three route
contracts (/health, /ready, /metrics).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 14: Update `CLAUDE.md`

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update the Architecture section's source tree**

Find the Architecture code block (search for `Source/CamSimTest/` in CLAUDE.md). Add a new line for the HttpServer directory, keeping it alphabetical within the existing list:

Replace:
```
      Metadata/                    # MISB ST 0601/ST 0102 KLV builder
      Sensor/                      # CPU post-process: EO/IR/NVG effects
```

With:
```
      HttpServer/                  # HTTP /health, /ready, /metrics (Phase 28G)
      Metadata/                    # MISB ST 0601/ST 0102 KLV builder
      Sensor/                      # CPU post-process: EO/IR/NVG effects
```

- [ ] **Step 2: Update the Environment section**

Find the Environment section (search for `Config via deploy/camsim_config.yaml or env vars`). Add these lines after the existing `CAMSIM_*` entries, before the "Full list" line:

```
- `CAMSIM_HTTP_SERVER_ENABLED` — HTTP health/metrics server toggle (default 1)
- `CAMSIM_HTTP_BIND_ADDR` — HTTP server bind address (default 0.0.0.0)
- `CAMSIM_HTTP_PORT` — HTTP server port (default 8910)
```

- [ ] **Step 3: Update the "Health" line in the Docker section**

Find the line:
```
- Health: `camsim_health.json` written every 90 ticks
```

Replace with:
```
- Health: HTTP `GET /health` / `GET /ready` on port 8910 (preferred), plus legacy `camsim_health.json` file (written every 90 ticks, kept for backward compatibility)
```

- [ ] **Step 4: Add the HTTP server to the Gotchas section**

Find the Gotchas section and add a new bullet at the end:

```
- **HTTP server is always-on by default.** Phase 28G adds an HTTP server on port 8910 serving `/health`, `/ready`, and `/metrics`. It binds to `0.0.0.0` by default — if you're running camsim on a multi-tenant host, set `CAMSIM_HTTP_BIND_ADDR=127.0.0.1` to restrict to loopback, or `CAMSIM_HTTP_SERVER_ENABLED=0` to disable entirely. The server is additive: the legacy `camsim_health.json` file writer and the `prometheus_metrics_path` textfile writer both still run.
```

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: update CLAUDE.md for Phase 28G HTTP health server

Adds HttpServer/ to the architecture source tree, documents the
new CAMSIM_HTTP_* env vars, updates the Docker health line, and
adds a gotcha explaining the default bind address and how to
disable the server.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 15: Update `Plan.md`

**Files:**
- Modify: `Plan.md`

- [ ] **Step 1: Locate the Phase 12D row and the Phase 28G row**

Search for `12D` and `28G` in `Plan.md`. Phase 12D is the existing Prometheus row marked done. Phase 28G is referenced in the line: *"Validation: CI passes all tests; health endpoint returns JSON; latency metrics in Prometheus"*.

- [ ] **Step 2: Add a new Phase entry for the HTTP health server**

In the phase table (the one that starts with `| 12D | Health & monitoring: health file, Prometheus metrics, IG mode in SOF | ✅ Done |` around line 21), add a new row after the last completed phase row. Use the same column format as existing rows:

```markdown
| 28G-1 | HTTP health & metrics server: /health, /ready, /metrics on port 8910 (replaces textfile-collector path for direct Grafana scrape) | ✅ Done |
```

If the table doesn't have a clear "last row," insert this row at the bottom of the phase table.

- [ ] **Step 3: Commit**

```bash
git add Plan.md
git commit -m "$(cat <<'EOF'
docs: mark Phase 28G-1 (HTTP health server) done in Plan.md

Records completion of the HTTP /health, /ready, /metrics endpoint
work that unblocks the sim-environment orchestrator Phase 6.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 16: Full test run + final smoke check

**Files:** none (verification only)

- [ ] **Step 1: Run all new HttpServer tests**

```bash
UnrealEditor "$(pwd)/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim.HttpServer+;Quit" \
  -unattended -nullrhi -nosound -stdout 2>&1 | tee /tmp/camsim-httpserver-all.log
grep -cE "Success" /tmp/camsim-httpserver-all.log
grep -cE "Failed"  /tmp/camsim-httpserver-all.log
```

Expected: at least 10 tests report Success (one for config defaults, two for HealthJson, two for Prometheus, five for Readiness, one for the Lifecycle integration test), zero Failed.

- [ ] **Step 2: Run the broader automation suite to confirm no regressions**

```bash
UnrealEditor "$(pwd)/unreal_project/CamSimTest/CamSimTest.uproject" \
  -ExecCmds="Automation RunTests CamSim+;Quit" \
  -unattended -nullrhi -nosound -stdout 2>&1 | tee /tmp/camsim-full-automation.log
grep -cE "Failed" /tmp/camsim-full-automation.log
```

Expected: zero Failed. If any non-HttpServer tests fail, they are pre-existing failures unrelated to this work — do NOT fix them in this plan.

- [ ] **Step 3: Launch smoke test with live HTTP probes**

```bash
./scripts/run.sh --headless &
CAMSIM_PID=$!

for i in $(seq 1 45); do
  if curl -fsS http://localhost:8910/health >/dev/null; then
    break
  fi
  sleep 1
done

curl -fsS http://localhost:8910/health >/dev/null || {
  echo "camsim HTTP server did not become reachable on :8910"
  kill $CAMSIM_PID 2>/dev/null
  wait $CAMSIM_PID 2>/dev/null
  exit 1
}

echo "=== /health ==="
curl -sS -w "\nHTTP %{http_code}\n" http://localhost:8910/health

echo "=== /ready ==="
curl -sS -w "\nHTTP %{http_code}\n" http://localhost:8910/ready

echo "=== /metrics (first 15 lines) ==="
curl -sS http://localhost:8910/metrics | head -15

kill $CAMSIM_PID 2>/dev/null
wait $CAMSIM_PID 2>/dev/null
```

Expected:
- `/health` → HTTP 200, JSON body with `"status":"ok"`
- `/ready` → HTTP 503 with `status:"not_ready"` reason body before readiness, or HTTP 200 with `status:"ready"` payload matching §10.4 once readiness gates pass
- `/metrics` → HTTP 200, Prometheus text containing §10.4 metric names/types (e.g., `camsim_render_fps`, `camsim_frame_drops_total`)

- [ ] **Step 4: No commit needed — verification step only.**

---

## Task 17: Update the source spec with a completion note

**Files:**
- Modify: `/opt/mac/sim-environment/sim-environment/docs/superpowers/specs/2026-04-10-dotnet-rest-orchestrator-design.md` (single-line status update in §10)

This task is in the sim-environment repo, not camera-simulator. It records that the cross-repo dependency has been satisfied so future readers of the orchestrator spec know Phase 6 is unblocked.

- [ ] **Step 1: Add a status line to the top of §10**

Open the sim-environment spec. Find `## 10. Cross-repo coordination — camera-simulator HTTP endpoint`. Insert a status line immediately after the heading:

```markdown
## 10. Cross-repo coordination — camera-simulator HTTP endpoint

**Status (updated <date>):** camera-simulator commit landed; see `camera-simulator` git log for the commit SHA. Phase 6 unblocked.
```

Replace `<date>` with today's date.

- [ ] **Step 2: Commit the sim-environment change**

```bash
cd /opt/mac/sim-environment/sim-environment
git add docs/superpowers/specs/2026-04-10-dotnet-rest-orchestrator-design.md
git commit -m "$(cat <<'EOF'
docs: mark camsim cross-repo dependency satisfied

Records that the camera-simulator HTTP health endpoint (Phase 28G)
has landed in the camera-simulator peer repo, unblocking the
orchestrator Phase 6 work in this repo.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
cd /opt/mac/sim-environment/camera-simulator  # return to the working repo
```

---

## Done

At this point the camera-simulator HTTP endpoint is complete, tested, documented, and the cross-repo dependency in the sim-environment spec is marked satisfied. The orchestrator Phase 6 plan (forthcoming) can proceed.

**What lives where:**
- `Source/CamSimTest/HttpServer/` — new module directory with `FCamSimHttpServer`, `HealthSnapshot`, `HealthJson`, `PrometheusFormatter`, `ReadinessEvaluator`.
- `Source/CamSimTest/Tests/HttpServer*.cpp` — 10+ automation tests.
- `Config/CamSimConfig.h/cpp` — `FHttpServerConfig` struct + YAML/env var loading.
- `Subsystem/CamSimSubsystem.{h,cpp}` — four new counter accessors, HTTP server started/stopped in lifecycle.
- `CamSimTest.Build.cs` — `HTTPServer` and `HTTP` module deps.
- `deploy/camsim_config.yaml`, `docs/configuration.md`, `CLAUDE.md`, `Plan.md` — documentation.

**What does NOT change (by design):**
- The existing `camsim_health.json` file writer continues to run (see `Subsystem/CamSimSubsystem.cpp:580-611`).
- The existing `prometheus_metrics_path` textfile writer continues to run (see `Subsystem/CamSimSubsystem.cpp:614-658`).
- No changes to CIGI, DIS, encoder, render, or any other runtime subsystem.
