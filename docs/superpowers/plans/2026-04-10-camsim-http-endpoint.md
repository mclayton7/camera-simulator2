# camera-simulator HTTP Health Endpoint — Adapt Existing Phase 28C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adapt the existing `FCamSimHealthServer` (merged from `phase-28-ops-hardening` into main as of commit `38bf757`) so the `sim-environment` REST orchestrator can probe camsim health over HTTP. Same endpoint works for both Docker Compose (orchestrator) and Kubernetes probes.

**Architecture:** The merge of `phase-28-ops-hardening` into main already brought the HTTP server, `FOperationalConfig`, YAML/env loading, graceful shutdown, and `/live` + `/ready` + `/metrics` routes. This plan is a narrow set of adaptations on top of that:
1. Add `HTTP` module to Build.cs for the integration test client
2. Replace the empty `/metrics` placeholder with a real Prometheus body using §10.4 metric names
3. Add `/health` as an alias route for `/live` so the orchestrator's generic "liveness" probe convention works alongside K8s's `/live`
4. Write an integration test using `FHttpModule` to GET `/health` against a live `FCamSimHealthServer`
5. Documentation updates

**Tech Stack:** UE5 C++ (same as existing Phase 28C code), `HTTPServer` + `HTTP` runtime modules, UE5 Automation Framework.

**Source spec:** `/opt/mac/sim-environment/sim-environment/docs/superpowers/specs/2026-04-10-dotnet-rest-orchestrator-design.md` §10.

**Prerequisite:** The `phase-28-ops-hardening` branch has been merged into `main` via merge commit `38bf757`. `FCamSimHealthServer` exists at `unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.{h,cpp}`. `FOperationalConfig` exists in `CamSimConfig.h` with `bHealthHttpEnabled` defaulting to `true` and `HealthHttpPort` defaulting to `8080`.

**Supersedes:** The original 17-task plan on this same path. The original was written without knowledge of the `phase-28-ops-hardening` branch. After discovering the existing work, this plan was rewritten to adapt rather than duplicate.

---

## Conventions

- All paths are **relative to** `/opt/mac/sim-environment/camera-simulator/`.
- Source files start with `// Copyright CamSim Contributors. All Rights Reserved.`.
- Run `scripts/check.sh` after each code change. Expect linker errors until `scripts/build_thirdparty.sh` is run once on this host — those are environmental, not plan failures. Compile errors (UBT rules, C++ syntax, type checks) are plan failures and must be fixed.
- Work on branch `feat/phase-28g-http-health-endpoint` which is currently at merged main (`38bf757`).
- Commits use `feat:` / `test:` / `docs:` prefix with `Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>` footer.
- Tests follow the existing UE5 Automation pattern (`IMPLEMENT_SIMPLE_AUTOMATION_TEST` with category `CamSim.HttpServer.*`).

---

## Key files

**Existing (merged from phase-28-ops-hardening, DO NOT recreate):**
- `unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.h` — `FCamSimHealthServer` struct with `TFunction` callback dependency injection
- `unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.cpp` — Route handlers for `/live`, `/ready`, `/metrics`
- `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` — `Initialize()` already constructs `FCamSimHealthServer` with callbacks via `Config.Operational.bHealthHttpEnabled`
- `unreal_project/CamSimTest/Source/CamSimTest/Config/CamSimConfig.h` — `FOperationalConfig` struct with `bHealthHttpEnabled = true` (default), `HealthHttpPort = 8080`
- `deploy/camsim_config.yaml` — `operational:` block already defined (from phase-28 merge)

**Files modified by this plan:**

| Path | Change |
|---|---|
| `unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs` | Add `"HTTP"` module dependency |
| `unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.h` | Expose a new `FStatusQueryFn` declared as `GetRenderFps`-style callbacks if needed for the real /metrics body |
| `unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.cpp` | Add `/health` route alias pointing at the same handler as `/live` |
| `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp` | Replace the empty `GetPrometheusMetrics` lambda returning `TEXT("")` with a real Prometheus body using §10.4 metric names |

**Files created:**

| Path | Purpose |
|---|---|
| `unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerLifecycleTest.cpp` | Integration test: start `FCamSimHealthServer` on test port, GET `/health` via `FHttpModule`, assert 200 + JSON body |

**Cross-repo edit (sim-environment):**

| Path | Change |
|---|---|
| `/opt/mac/sim-environment/sim-environment/docs/superpowers/specs/2026-04-10-dotnet-rest-orchestrator-design.md` | §10 status line: camsim HTTP endpoint landed via merge 38bf757, port 8080 not 8910, `/health` alias added alongside existing `/live` |

**Files updated in camsim for docs:**
- `docs/configuration.md` — the existing Phase 28 content likely already documents `operational.health_http_*`; verify and add a note about the `/health` alias and §10.4 metric names
- `CLAUDE.md` — port table should mention 8080 for HTTP health
- `Plan.md` — the Phase 28 completion row likely already exists; add an inline note about the `/health` alias and real /metrics body

---

## §10.4 metric contract (authoritative for Task 3)

The sim-environment orchestrator spec §10.4 defines the Prometheus exposition format metric names the `/metrics` endpoint must emit:

**Gauges:**
- `camsim_render_fps` — current render thread framerate
- `camsim_output_fps` — current encoder output framerate
- `camsim_entity_count` — number of active entities in the scene
- `camsim_uptime_seconds` — subsystem uptime in seconds

**Counters:**
- `camsim_frame_drops_total` — total frame drops across all categories
- `camsim_cigi_packets_total` — total CIGI packets received
- `camsim_dis_packets_total` — total DIS PDUs received
- `camsim_frames_encoded_total` — total frames successfully encoded

**Histograms (optional, emit only if `Config.Performance.bTrackPipelineLatency` is true):**
- `camsim_frame_latency_ms` — per-frame latency P50/P95/P99 from `FPipelineLatencyTracker`

Every metric has a `# HELP` and `# TYPE` line per Prometheus text format 0.0.4. The response body ends with a single newline.

---

## Task 1: Add `"HTTP"` module to Build.cs

The merge already added `"HTTPServer"` via commit 243a66a. This task adds `"HTTP"` as well so the integration test in Task 5 can use `FHttpModule::Get().CreateRequest()` to make client requests against the running server.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs`

- [ ] **Step 1: Add `"HTTP"` to `PublicDependencyModuleNames`**

Find the existing `"HTTPServer"` entry (added by commit 243a66a near the end of the module list, after `"Niagara"` / `"NiagaraCore"`) and add `"HTTP"` immediately after it:

```csharp
			// HTTP health endpoints (Phase 28C)
			"HTTPServer",
			// HTTP client for automation tests of the HTTP server
			"HTTP",
```

- [ ] **Step 2: Verify build**

Run: `scripts/check.sh`

Expected: no new compile errors. Linker errors for `libcigicl.a` / FFmpeg libs are pre-existing and environmental; ignore them.

- [ ] **Step 3: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/CamSimTest.Build.cs
git commit -m "$(cat <<'EOF'
build: add HTTP client module for automation tests

Companion to the HTTPServer dep added in Phase 28C. HTTP exposes
FHttpModule::Get().CreateRequest() which the upcoming HTTP health
server integration test uses to drive GET /health.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Implement real `/metrics` body with §10.4 metric names

The merged Phase 28C code uses a `TFunction<FString()>` callback for `/metrics` body generation, and `CamSimSubsystem::Initialize` currently wires it to a placeholder that returns `TEXT("")`. This task replaces that placeholder with a real Prometheus exposition body using the metric names from §10.4.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp`

- [ ] **Step 1: Locate the existing placeholder**

Search for the text `GetPrometheusMetrics — placeholder` or find the line inside `Impl->HealthServer->Start(...)` that reads:

```cpp
// GetPrometheusMetrics — placeholder; /metrics endpoint can be improved in a follow-up
[ImplPtr]() -> FString
{
    return TEXT("");
}
```

- [ ] **Step 2: Replace the placeholder with a real Prometheus body generator**

Replace the lambda body with one that reads counters from `ImplPtr` and emits the §10.4 metric contract:

```cpp
// §10.4 contract: gauges render_fps, output_fps, entity_count,
// uptime_seconds; counters frame_drops_total, cigi_packets_total,
// dis_packets_total, frames_encoded_total. Histogram frame_latency_ms
// only when bTrackPipelineLatency is enabled.
[ImplPtr, this]() -> FString
{
    // Resolve per-tick state read-only. All of these accessors are safe
    // off the game thread because they read atomic POD counters only.
    const uint64 EncodedFrames = (ImplPtr->VideoEncoder && ImplPtr->VideoEncoder->IsOpen())
        ? ImplPtr->VideoEncoder->GetSuccessfulFrameCount() : 0;
    const uint64 CigiRx = ImplPtr->CigiReceiver
        ? ImplPtr->CigiReceiver->GetReceivedPacketCount() : 0;
    const uint64 DisRx = ImplPtr->DisReceiver
        ? ImplPtr->DisReceiver->GetReceivedPacketCount() : 0;
    const double Uptime = FPlatformTime::Seconds() - ImplPtr->StartTimeSec;
    const int32 EntityCount = ImplPtr->EntityManager
        ? ImplPtr->EntityManager->GetEntityCount() : 0;

    // Frame drops: sum across categories from the camera's FFrameDropStats.
    int32 FrameDropsTotal = 0;
    double RenderFps = 0.0;
    double OutputFps = 0.0;
    if (ACamSimCamera* Cam = Camera_.Get())
    {
        if (Cam->IsTrackingFrameDrops())
        {
            const FFrameDropStats& D = Cam->GetFrameDropStats();
            FrameDropsTotal = D.Total();
        }
        RenderFps = Cam->GetCurrentRenderFps();
        OutputFps = Cam->GetCurrentOutputFps();
    }

    FString Body;
    Body.Reserve(2048);

    Body += TEXT("# HELP camsim_render_fps Current render thread framerate.\n");
    Body += TEXT("# TYPE camsim_render_fps gauge\n");
    Body += FString::Printf(TEXT("camsim_render_fps %.3f\n"), RenderFps);

    Body += TEXT("# HELP camsim_output_fps Current encoder output framerate.\n");
    Body += TEXT("# TYPE camsim_output_fps gauge\n");
    Body += FString::Printf(TEXT("camsim_output_fps %.3f\n"), OutputFps);

    Body += TEXT("# HELP camsim_entity_count Active entities in the scene.\n");
    Body += TEXT("# TYPE camsim_entity_count gauge\n");
    Body += FString::Printf(TEXT("camsim_entity_count %d\n"), EntityCount);

    Body += TEXT("# HELP camsim_uptime_seconds Subsystem uptime in seconds.\n");
    Body += TEXT("# TYPE camsim_uptime_seconds gauge\n");
    Body += FString::Printf(TEXT("camsim_uptime_seconds %.3f\n"), Uptime);

    Body += TEXT("# HELP camsim_frame_drops_total Total frame drops across all categories.\n");
    Body += TEXT("# TYPE camsim_frame_drops_total counter\n");
    Body += FString::Printf(TEXT("camsim_frame_drops_total %d\n"), FrameDropsTotal);

    Body += TEXT("# HELP camsim_cigi_packets_total Total CIGI packets received.\n");
    Body += TEXT("# TYPE camsim_cigi_packets_total counter\n");
    Body += FString::Printf(TEXT("camsim_cigi_packets_total %llu\n"), CigiRx);

    Body += TEXT("# HELP camsim_dis_packets_total Total DIS PDUs received.\n");
    Body += TEXT("# TYPE camsim_dis_packets_total counter\n");
    Body += FString::Printf(TEXT("camsim_dis_packets_total %llu\n"), DisRx);

    Body += TEXT("# HELP camsim_frames_encoded_total Total frames successfully encoded.\n");
    Body += TEXT("# TYPE camsim_frames_encoded_total counter\n");
    Body += FString::Printf(TEXT("camsim_frames_encoded_total %llu\n"), EncodedFrames);

    // Optional histogram: only emit when latency tracking is enabled.
    if (ImplPtr->LatencyTracker)
    {
        const auto Percentiles = ImplPtr->LatencyTracker->ComputePercentiles();
        Body += TEXT("# HELP camsim_frame_latency_ms Per-frame pipeline latency.\n");
        Body += TEXT("# TYPE camsim_frame_latency_ms summary\n");
        Body += FString::Printf(TEXT("camsim_frame_latency_ms{quantile=\"0.5\"} %.3f\n"),
            Percentiles.TotalP50Ms);
        Body += FString::Printf(TEXT("camsim_frame_latency_ms{quantile=\"0.95\"} %.3f\n"),
            Percentiles.TotalP95Ms);
        Body += FString::Printf(TEXT("camsim_frame_latency_ms{quantile=\"0.99\"} %.3f\n"),
            Percentiles.TotalP99Ms);
    }

    return Body;
}
```

- [ ] **Step 3: Verify accessor availability**

Before running the build, confirm that the following accessors exist on the referenced types. Each is called by the lambda above; if any is missing, the subagent executing this task should ADD it via a narrow public getter on the owning class (following the same pattern as `GetFrameDropStats`) rather than inventing a new way to compute the value:

| Required accessor | Likely owning class | Action if missing |
|---|---|---|
| `FCigiReceiver::GetReceivedPacketCount() const` | existing, was used by file-based .prom writer | no action |
| `FDisReceiver::GetReceivedPacketCount() const` | should exist — phase-28 work | add if missing |
| `IFrameSink::IsOpen() const` / `::GetSuccessfulFrameCount() const` | existing | no action |
| `FCamSimEntityManager::GetEntityCount() const` | check — may need to be added | add as public const getter that returns the manager's entity count |
| `ACamSimCamera::GetCurrentRenderFps() const` / `::GetCurrentOutputFps() const` | likely from Phase 28 FPS tracking | check — if `FPerformanceConfig.bTrackPipelineLatency` exists but render/output FPS tracking does not, add a simple 1Hz-averaged FPS calculation inside `ACamSimCamera::Tick` and expose via getter |
| `FPipelineLatencyTracker::ComputePercentiles()` returning a struct with `TotalP50Ms/P95Ms/P99Ms` fields | existing Phase 28G | field names may differ — use what's there and adapt the Printf |

**If any required field cannot be cleanly resolved**, the implementer should report DONE_WITH_CONCERNS after doing what they can, and leave a comment in the lambda explaining which gauge/counter could not be wired. The plan-level design accepts a partial `/metrics` body rather than blocking on missing subsystem instrumentation.

- [ ] **Step 4: Run the compile check**

```bash
scripts/check.sh
```
Expected: C++ compile succeeds. Linker errors for ThirdParty libs ignored.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Subsystem/CamSimSubsystem.cpp
git commit -m "$(cat <<'EOF'
feat(28C): implement real /metrics body with §10.4 metric contract

Replaces the empty-string placeholder GetPrometheusMetrics callback
in UCamSimSubsystem::Initialize with a Prometheus exposition body
that matches the sim-environment orchestrator spec §10.4 metric
contract: gauges render_fps, output_fps, entity_count,
uptime_seconds; counters frame_drops_total, cigi_packets_total,
dis_packets_total, frames_encoded_total. Optional histogram
frame_latency_ms emitted only when pipeline latency tracking is
enabled.

Consumed by both Grafana direct scrapes and the sim-environment
REST orchestrator's HealthMonitor.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add `/health` route alias for `/live`

The sim-environment orchestrator spec §10.4 and the orchestrator Phase 6 plan call for a `GET /health` probe. The existing `FCamSimHealthServer` exposes `GET /live` (K8s liveness convention). This task adds `/health` as a second route bound to the same handler, so both conventions work without breaking existing K8s manifests.

**Files:**
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.h` — add `FHttpRouteHandle HealthRouteHandle;` alongside the existing handle members
- Modify: `unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.cpp` — bind `/health` using the same handler lambda as `/live`, unbind it in `Stop()`

- [ ] **Step 1: Add `HealthRouteHandle` member to the header**

Open `CamSimHealthServer.h`. Find the `private:` block at the bottom that holds the existing route handle members (or the `TSharedPtr<IHttpRouter> Router`). The existing code uses `BindRoute` without storing the returned `FHttpRouteHandle`; the server's `Stop()` relies on `StopAllListeners()` rather than per-route unbinds.

**Important:** verify how the existing code stores route handles by reading the file first. If the existing pattern is "don't store route handles, just call StopAllListeners", follow that same pattern for `/health` — no new member needed. If the existing pattern stores handles per route, store `HealthRouteHandle` too.

Decide based on what the existing file does. Pick the option that minimizes diff.

- [ ] **Step 2: Extract the `/live` handler as a lambda once, bind it twice**

In `CamSimHealthServer.cpp::Start`, the existing code has a `/live` route bound with an inline lambda. Refactor to name the lambda once and reuse it for both `/live` and `/health`:

```cpp
// Shared handler — used for both /live (K8s convention) and /health
// (sim-environment REST orchestrator convention). Both names probe the
// same watchdog — returns 200 when the game loop ticked within 5s,
// 503 with a stall duration otherwise.
auto LivenessHandler = FHttpRequestHandler::CreateLambda(
    [this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
    {
        const double AgeSec = FPlatformTime::Seconds() - LastTickTimeSec;
        if (AgeSec < 5.0)
        {
            auto Response = FHttpServerResponse::Create(
                FString(TEXT("{\"status\":\"ok\"}")), TEXT("application/json"));
            OnComplete(MoveTemp(Response));
        }
        else
        {
            FString Body = FString::Printf(
                TEXT("{\"status\":\"stalled\",\"last_tick_ago_s\":%.1f}"), AgeSec);
            auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
            Response->Code = EHttpServerResponseCodes::ServiceUnavail;
            OnComplete(MoveTemp(Response));
        }
        return true;
    });

Router->BindRoute(FHttpPath(TEXT("/live")),   EHttpServerRequestVerbs::VERB_GET, LivenessHandler);
Router->BindRoute(FHttpPath(TEXT("/health")), EHttpServerRequestVerbs::VERB_GET, LivenessHandler);
```

- [ ] **Step 3: Update the log line to include /health**

In the same file, the existing `Start()` ends with:

```cpp
UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: listening on port %d (/live /ready /metrics)"), Port);
```

Update to:

```cpp
UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: listening on port %d (/live /health /ready /metrics)"), Port);
```

- [ ] **Step 4: Verify build**

```bash
scripts/check.sh
```
Expected: exit 0 compile-wise.

- [ ] **Step 5: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.h \
        unreal_project/CamSimTest/Source/CamSimTest/Health/CamSimHealthServer.cpp
git commit -m "$(cat <<'EOF'
feat(28C): add /health route alias for /live

Binds a second GET /health route to the same liveness handler as
/live so the sim-environment REST orchestrator can use its generic
/health convention without breaking K8s probes that target /live.
Same watchdog semantics (200 if Tick() fired within 5s, 503 with
stall duration otherwise).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Integration test — start server, GET /health via `FHttpModule`

A UE5 Automation test that proves the `/health` alias works end-to-end. Starts an `FCamSimHealthServer` on a test-only port (48080, high enough to avoid collision), calls `UpdateTick()` once to arm the liveness watchdog, makes a real HTTP GET with the `HTTP` client module, manually ticks `HttpManager`, and asserts `200` + `status:"ok"`.

**Files:**
- Create: `unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerLifecycleTest.cpp`

- [ ] **Step 1: Create the test file**

```cpp
// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Health/CamSimHealthServer.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IHttpRequest.h"

// -------------------------------------------------------------------------
// CamSimHealthServer Lifecycle Integration Test
//
// Starts FCamSimHealthServer on a test-only port, calls UpdateTick() to
// arm the liveness watchdog, makes a real HTTP GET to /health via
// HttpModule, and asserts the 200 + status:ok JSON body. Manual HttpManager
// tick required because automation tests don't receive normal game-thread
// ticks.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerLifecycleHealthAliasTest,
	"CamSim.HttpServer.Lifecycle.HealthAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerLifecycleHealthAliasTest::RunTest(const FString& Parameters)
{
	// Test-only port — chosen high enough to avoid collision with any
	// running camsim or the sim-environment orchestrator.
	constexpr int32 TestPort = 48080;

	FCamSimHealthServer Server;
	const bool bStarted = Server.Start(TestPort,
		/*IsAlive*/        [](){ return true; },
		/*IsEncoderReady*/ [](){ return true; },
		/*IsCigiReady*/    [](){ return true; },
		/*HasFirstFrame*/  [](){ return true; },
		/*GetPrometheusMetrics*/ []() -> FString { return TEXT(""); });

	if (!TestTrue(TEXT("server started"), bStarted))
	{
		return false;
	}

	// Arm the watchdog so /live and /health return 200 rather than 503.
	Server.UpdateTick();

	// Make the GET via the HTTP client module.
	FHttpModule& HttpModule = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = HttpModule.CreateRequest();
	Request->SetVerb(TEXT("GET"));
	Request->SetURL(FString::Printf(TEXT("http://127.0.0.1:%d/health"), TestPort));
	Request->SetTimeout(5.0);

	bool bCompleted = false;
	int32 StatusCode = 0;
	FString Body;

	Request->OnProcessRequestComplete().BindLambda(
		[&bCompleted, &StatusCode, &Body](
			FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
		{
			bCompleted = true;
			if (Resp.IsValid())
			{
				StatusCode = Resp->GetResponseCode();
				Body = Resp->GetContentAsString();
			}
		});

	Request->ProcessRequest();

	// Spin the HTTP manager for up to 6 seconds. Automation tests do not
	// receive game-thread ticks, so we drive the manager manually.
	const double Deadline = FPlatformTime::Seconds() + 6.0;
	while (!bCompleted && FPlatformTime::Seconds() < Deadline)
	{
		HttpModule.GetHttpManager().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}

	Server.Stop();

	TestTrue(TEXT("request completed"), bCompleted);
	TestEqual(TEXT("status code 200"), StatusCode, 200);
	TestTrue(TEXT("body contains status ok"), Body.Contains(TEXT("\"status\":\"ok\"")));

	return true;
}
```

- [ ] **Step 2: Verify compile**

```bash
scripts/check.sh
```
Expected: exit 0 compile-wise.

- [ ] **Step 3: Run the test if the UE5 editor and ThirdParty libs are available**

If `scripts/build_thirdparty.sh` has been run on this host and `libcigicl.a` / `libav*.a` exist, run:

```bash
UnrealEditor unreal_project/CamSimTest/CamSimTest.uproject \
  -ExecCmds="Automation RunTests CamSim.HttpServer.Lifecycle+;Quit" \
  -unattended -nullrhi -nosound -stdout 2>&1 | tee /tmp/camsim-lifecycle-test.log
grep -E "Success|Failed" /tmp/camsim-lifecycle-test.log
```
Expected: `CamSim.HttpServer.Lifecycle.HealthAlias` reports Success.

**If ThirdParty libs are missing and the test cannot run**, report DONE_WITH_CONCERNS — the test code compiles cleanly, and the test itself will run once the host's ThirdParty libs are built. Do NOT try to fix the ThirdParty lib issue as part of this task; it's environmental.

- [ ] **Step 4: Commit**

```bash
git add unreal_project/CamSimTest/Source/CamSimTest/Tests/HttpServerLifecycleTest.cpp
git commit -m "$(cat <<'EOF'
test(28C): add integration test for /health alias via HttpModule

Starts FCamSimHealthServer on a test port, arms the liveness
watchdog via UpdateTick(), makes a real HTTP GET to /health, and
asserts 200 + status:ok. Uses the HTTP client module added in the
previous Build.cs commit. Manual HttpManager.Tick() loop required
because automation tests don't receive game-thread ticks.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Update camera-simulator documentation

**Files:**
- Modify: `docs/configuration.md`
- Modify: `CLAUDE.md`
- Modify: `Plan.md`

- [ ] **Step 1: Update `docs/configuration.md`**

Find the existing Phase 28 operational section (added by the merge). It should already document `operational.health_http_enabled` and `operational.health_http_port`. Add a short subsection on routes — or update if a routes subsection exists but omits `/health`:

```markdown
### HTTP Health Server Routes

When `operational.health_http_enabled = true` (the default), the server binds the following routes on `operational.health_http_port` (default 8080):

| Route | Purpose | Returns |
|---|---|---|
| `GET /live` | Kubernetes liveness convention — watchdog on game-thread Tick() | 200 `{"status":"ok"}` if Tick fired within 5s, 503 `{"status":"stalled","last_tick_ago_s":N}` otherwise |
| `GET /health` | sim-environment orchestrator convention — alias for `/live` (same handler) | Same as `/live` |
| `GET /ready` | Readiness: encoder open AND CIGI packet received AND first frame encoded | 200 `{"status":"ready",...}` or 503 `{"status":"not_ready",...}` |
| `GET /metrics` | Prometheus exposition format | 200 text/plain with `camsim_*` gauges and counters |

**Metric names** (Prometheus 0.0.4 exposition format):
- Gauges: `camsim_render_fps`, `camsim_output_fps`, `camsim_entity_count`, `camsim_uptime_seconds`
- Counters: `camsim_frame_drops_total`, `camsim_cigi_packets_total`, `camsim_dis_packets_total`, `camsim_frames_encoded_total`
- Histograms (when `performance.track_pipeline_latency = true`): `camsim_frame_latency_ms`
```

- [ ] **Step 2: Update `CLAUDE.md`**

Find the Environment section and the Gotchas section. In the Environment section, confirm `CAMSIM_HEALTH_HTTP_ENABLED` / `CAMSIM_HEALTH_HTTP_PORT` are documented (they should be — merged from phase-28). Add a new gotcha if one is not already present:

```markdown
- **HTTP health server is on by default.** `operational.health_http_enabled` defaults to `true` for compatibility with the sim-environment REST orchestrator. The server binds port 8080 by default on `0.0.0.0`. To disable entirely: `CAMSIM_HEALTH_HTTP_ENABLED=0`. To bind loopback only (single-tenant dev): `CAMSIM_HEALTH_HTTP_PORT=8080` and rebind via reverse proxy, since `FHttpServerModule::GetHttpRouter` takes only a port (no bind address). The routes `/live` and `/health` are aliases — both probe the same game-loop watchdog.
```

- [ ] **Step 3: Update `Plan.md`**

Find the Phase 28C row (already added by the merge). Add a short note on the `/health` alias and real `/metrics` body. If there's a status column, keep it as Done. Example:

```markdown
| **28C** HTTP Health Endpoints      | `/live`, `/health`, `/ready`, `/metrics` on :8080; §10.4 metric names; used by K8s and by sim-environment orchestrator | M      | ✅ Sprint 1 Done (alias + metrics body added 2026-04-11) |
```

- [ ] **Step 4: Commit**

```bash
git add docs/configuration.md CLAUDE.md Plan.md
git commit -m "$(cat <<'EOF'
docs: document /health alias and §10.4 /metrics body

- configuration.md: new HTTP Health Server Routes table with all
  four endpoints, metric names per §10.4 of the sim-environment
  orchestrator spec.
- CLAUDE.md: gotcha on default-on HTTP server, bind address
  behavior, and the /live ↔ /health alias.
- Plan.md: Phase 28C row updated with the alias and metrics
  completion note.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Cross-repo — mark sim-environment spec §10 satisfied

**Files:**
- Modify: `/opt/mac/sim-environment/sim-environment/docs/superpowers/specs/2026-04-10-dotnet-rest-orchestrator-design.md`

This task updates the sim-environment design spec to reflect that the camsim HTTP endpoint landed, so future readers of the orchestrator spec know Phase 6 is unblocked.

- [ ] **Step 1: Open the spec file**

`/opt/mac/sim-environment/sim-environment/docs/superpowers/specs/2026-04-10-dotnet-rest-orchestrator-design.md`

- [ ] **Step 2: Update §10 status line and port references**

Find `## 10. Cross-repo coordination — camera-simulator HTTP endpoint`. Immediately after the heading, add or replace the status line to read:

```markdown
**Status (updated 2026-04-11):** camsim HTTP endpoint landed on main via merge commit 38bf757 (bringing phase-28-ops-hardening Phase 28C into main). The sim-environment orchestrator's HealthMonitor can probe `http://localhost:8080/health` with `/live` as an alias for K8s compatibility. Default port is 8080, not 8910 as originally specified. Phase 6 unblocked.
```

Then search for other references to port `8910` in §10 and update to `8080`:

```markdown
# old
/health endpoint at http://localhost:8910/health

# new
/health endpoint at http://localhost:8080/health
```

Also update the port in the compose-example env var in §7.2 if it reads `SIM_CAMSIM_HTTP_PORT=8910`:

```markdown
# new
- SIM_CAMSIM_HTTP_PORT=8080
```

- [ ] **Step 3: Commit in the sim-environment repo**

```bash
cd /opt/mac/sim-environment/sim-environment
git add docs/superpowers/specs/2026-04-10-dotnet-rest-orchestrator-design.md
git commit -m "$(cat <<'EOF'
docs: mark camsim HTTP cross-repo dependency satisfied

camsim merge commit 38bf757 brought phase-28-ops-hardening into
main, landing the existing FCamSimHealthServer implementation.
Port is 8080 (not 8910 as originally specified). /health is an
alias for /live. The sim-environment REST orchestrator Phase 6
HealthMonitor can now probe http://localhost:8080/health and
http://localhost:8080/ready.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
cd /opt/mac/sim-environment/camera-simulator
```

---

## Done

At this point:
- `HTTP` module is available for automation test clients
- `/metrics` emits real Prometheus body with §10.4 metric names
- `/health` exists as an alias for `/live` (same handler)
- Integration test verifies the alias works via a real HTTP GET
- Docs in both repos reflect the current state
- sim-environment Phase 6 (orchestrator core) is unblocked on the camsim side

**What was NOT changed by this plan** (remains from the Phase 28C merge):
- `FCamSimHealthServer` struct and its `TFunction` callback contract
- `FOperationalConfig` fields and YAML/env loading
- `/live` watchdog semantics (5-second threshold)
- `/ready` readiness gates (encoder + CIGI + first frame)
- Subsystem lifecycle wiring (construction in `Initialize`, teardown in Pimpl destructor and SIGTERM delegate)
- Graceful shutdown behavior
- `deploy/camsim_config.yaml` `operational:` block (already in place)
