// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Health/CamSimHealthServer.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IHttpRequest.h"

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
