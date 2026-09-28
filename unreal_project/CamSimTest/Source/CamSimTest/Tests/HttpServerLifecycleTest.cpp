// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Health/CamSimHealthServer.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IHttpRequest.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

// -------------------------------------------------------------------------
// CamSimHealthServer Lifecycle Integration Test
//
// Starts FCamSimHealthServer on a test-only port, calls UpdateTick() to
// arm the liveness watchdog, makes a real HTTP GET to /health via
// HttpModule, and asserts the 200 + status:ok JSON body. Manual
// HttpManager.Tick() loop is required because automation tests don't
// receive normal game-thread ticks.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerLifecycleHealthAliasTest,
    "CamSim.HttpServer.Lifecycle.HealthAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerLifecycleHealthAliasTest::RunTest(const FString& Parameters)
{
    // Test-only port — high enough to avoid collision with any running
    // camsim instance or the sim-environment orchestrator (8080).
    constexpr int32 TestPort = 48080;

    FCamSimHealthServer Server;
    const bool bStarted = Server.Start(TestPort,
        /*IsAlive*/        [](){ return true; },
        /*IsEncoderReady*/ [](){ return true; },
        /*IsSensorGraphReady*/ [](){ return true; },
        /*IsCigiReady*/    [](){ return true; },
        /*HasFirstFrame*/  [](){ return true; },
        /*IsTerrainReady*/ [](){ return true; },
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

    // Spin the HTTP client manager AND the core ticker for up to 6 seconds.
    // Automation tests do not receive game-thread ticks, so we drive both
    // manually: the FHttpManager for client-side request/response pumping,
    // and FTSTicker so FHttpServerModule's internal listener ticks run.
    const double Deadline = FPlatformTime::Seconds() + 6.0;
    while (!bCompleted && FPlatformTime::Seconds() < Deadline)
    {
        HttpModule.GetHttpManager().Tick(0.01f);
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.01f);
    }

    Server.Stop();

    TestTrue(TEXT("request completed"), bCompleted);
    TestEqual(TEXT("status code 200"), StatusCode, 200);
    TestTrue(TEXT("body contains status ok"), Body.Contains(TEXT("\"status\":\"ok\"")));

    return true;
}

// -------------------------------------------------------------------------
// Port busy at start (e.g. TIME_WAIT from a run restarted within ~30 s):
// the server must not report listening, and must come up by itself once the
// port frees. A plain listening socket holds the port here.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerLifecyclePortBusyTest,
    "CamSim.HttpServer.Lifecycle.PortBusyRetriesUntilFree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerLifecyclePortBusyTest::RunTest(const FString& Parameters)
{
    constexpr int32 TestPort = 48082;

    ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FSocket* Blocker = Sockets->CreateSocket(NAME_Stream, TEXT("PortBlocker"), false);
    TSharedRef<FInternetAddr> Addr = Sockets->CreateInternetAddr();
    Addr->SetAnyAddress();
    Addr->SetPort(TestPort);
    if (!TestTrue(TEXT("blocker bound"), Blocker->Bind(*Addr) && Blocker->Listen(1)))
    {
        Sockets->DestroySocket(Blocker);
        return false;
    }

    // The engine logs the failed bind as an error; that is the signal under test.
    AddExpectedMessage(TEXT("unable to bind to 0.0.0.0:48082"), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedMessage(TEXT("failed to start listening on port 48082"), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedMessage(TEXT("port 48082 is busy"), EAutomationExpectedErrorFlags::Contains, 1);

    FCamSimHealthServer Server;
    Server.Start(TestPort,
        [](){ return true; }, [](){ return true; }, [](){ return true; },
        [](){ return true; }, [](){ return true; }, [](){ return true; },
        []() -> FString { return TEXT(""); });
    TestFalse(TEXT("not listening while the port is taken"), Server.IsListening());

    Blocker->Close();
    Sockets->DestroySocket(Blocker);

    // The retry runs on the core ticker; one tick past the retry interval.
    FTSTicker::GetCoreTicker().Tick(FCamSimHealthServer::BindRetryIntervalSec + 0.1f);
    TestTrue(TEXT("listening once the port is free"), Server.IsListening());

    Server.UpdateTick();
    FHttpModule& HttpModule = FHttpModule::Get();
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = HttpModule.CreateRequest();
    Request->SetVerb(TEXT("GET"));
    Request->SetURL(FString::Printf(TEXT("http://127.0.0.1:%d/live"), TestPort));
    Request->SetTimeout(5.0);
    bool bCompleted = false;
    int32 StatusCode = 0;
    Request->OnProcessRequestComplete().BindLambda(
        [&bCompleted, &StatusCode](FHttpRequestPtr, FHttpResponsePtr Resp, bool)
        {
            bCompleted = true;
            StatusCode = Resp.IsValid() ? Resp->GetResponseCode() : 0;
        });
    Request->ProcessRequest();
    const double Deadline = FPlatformTime::Seconds() + 6.0;
    while (!bCompleted && FPlatformTime::Seconds() < Deadline)
    {
        HttpModule.GetHttpManager().Tick(0.01f);
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.01f);
    }
    Server.Stop();

    TestEqual(TEXT("/live answers 200 after the retry"), StatusCode, 200);
    return true;
}

// -------------------------------------------------------------------------
// /ready reports the GPU sensor graph as its own gate ("sensor_graph"),
// separate from "encoder": without the graph no frames are produced, so
// /ready is 503 even when every other gate passes.
// -------------------------------------------------------------------------

namespace
{
    /** GET http://127.0.0.1:<Port><Path>, pumping both tickers (see CLAUDE.md). */
    bool HttpGet(int32 Port, const TCHAR* Path, int32& OutCode, FString& OutBody)
    {
        FHttpModule& HttpModule = FHttpModule::Get();
        TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = HttpModule.CreateRequest();
        Request->SetVerb(TEXT("GET"));
        Request->SetURL(FString::Printf(TEXT("http://127.0.0.1:%d%s"), Port, Path));
        Request->SetTimeout(5.0);
        bool bCompleted = false;
        Request->OnProcessRequestComplete().BindLambda(
            [&bCompleted, &OutCode, &OutBody](FHttpRequestPtr, FHttpResponsePtr Resp, bool)
            {
                bCompleted = true;
                if (Resp.IsValid()) { OutCode = Resp->GetResponseCode(); OutBody = Resp->GetContentAsString(); }
            });
        Request->ProcessRequest();
        const double Deadline = FPlatformTime::Seconds() + 6.0;
        while (!bCompleted && FPlatformTime::Seconds() < Deadline)
        {
            HttpModule.GetHttpManager().Tick(0.01f);
            FTSTicker::GetCoreTicker().Tick(0.01f);
            FPlatformProcess::Sleep(0.01f);
        }
        return bCompleted;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpServerReadySensorGraphTest,
    "CamSim.HttpServer.Lifecycle.ReadyReportsSensorGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FHttpServerReadySensorGraphTest::RunTest(const FString& Parameters)
{
    constexpr int32 TestPort = 48083;
    bool bSensorGraph = false;

    FCamSimHealthServer Server;
    const bool bStarted = Server.Start(TestPort,
        /*IsAlive*/            [](){ return true; },
        /*IsEncoderReady*/     [](){ return true; },
        /*IsSensorGraphReady*/ [&bSensorGraph](){ return bSensorGraph; },
        /*IsCigiReady*/        [](){ return true; },
        /*HasFirstFrame*/      [](){ return true; },
        /*IsTerrainReady*/     [](){ return true; },
        /*GetPrometheusMetrics*/ []() -> FString { return TEXT(""); });
    if (!TestTrue(TEXT("server started"), bStarted))
    {
        return false;
    }

    int32 Code = 0;
    FString Body;
    TestTrue(TEXT("request without sensor graph completed"), HttpGet(TestPort, TEXT("/ready"), Code, Body));
    TestEqual(TEXT("no sensor graph: 503"), Code, 503);
    TestTrue(TEXT("no sensor graph: sensor_graph false"), Body.Contains(TEXT("\"sensor_graph\":false")));
    TestTrue(TEXT("no sensor graph: encoder reported on its own"), Body.Contains(TEXT("\"encoder\":true")));

    bSensorGraph = true;
    Code = 0; Body.Reset();
    TestTrue(TEXT("request with sensor graph completed"), HttpGet(TestPort, TEXT("/ready"), Code, Body));
    TestEqual(TEXT("all gates: 200"), Code, 200);
    TestTrue(TEXT("all gates: sensor_graph true"), Body.Contains(TEXT("\"sensor_graph\":true")));
    TestTrue(TEXT("all gates: ready"), Body.Contains(TEXT("\"status\":\"ready\"")));

    Server.Stop();
    return true;
}
