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
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"

// -------------------------------------------------------------------------
// ROADMAP 3A: GET /snapshot. Unbound route is 404; a request with no frame
// times out as 503; an offered frame comes back as a PNG of the same size.
// Both tickers are pumped (see CLAUDE.md "HTTP automation tests").
// -------------------------------------------------------------------------

namespace
{
	// One port for all snapshot tests: FHttpServerModule keeps a router per
	// port and StartAllListeners() re-binds every port used so far.
	constexpr int32 SnapshotTestPort = 48081;

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

	/**
	 * Several HTTP tests in one session re-listen on ports that still hold
	 * TIME_WAIT sockets (StartAllListeners restarts every port used so far),
	 * which fails without SO_REUSEADDR. Enable it for tests only, through the
	 * config-changed path HTTPServer uses to refresh its listener cache.
	 * Production keeps UE's default: reuse also sets SO_REUSEPORT, which would
	 * let two CamSim instances share the health port silently.
	 */
	void EnableListenerReuseForTests()
	{
		static bool bDone = false;
		if (bDone) return;
		bDone = true;
		GConfig->SetBool(TEXT("HTTPServer.Listeners"), TEXT("DefaultReuseAddressAndPort"), true, GEngineIni);
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("HTTPServer.Listeners") });
	}

	bool StartServer(FCamSimHealthServer& Server, int32 Port)
	{
		EnableListenerReuseForTests();
		auto True = [](){ return true; };
		return Server.Start(Port, True, True, True, True, True, []() -> FString { return TEXT(""); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotUnboundTest,
	"CamSim.Health.Snapshot.UnboundIs404",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSnapshotUnboundTest::RunTest(const FString& Parameters)
{
	constexpr int32 Port = SnapshotTestPort;
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
	constexpr int32 Port = SnapshotTestPort;
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
	constexpr int32 Port = SnapshotTestPort;
	constexpr int32 W = 64, H = 36;
	FCamSimSnapshotService Service;
	FCamSimHealthServer Server;
	if (!TestTrue(TEXT("started"), StartServer(Server, Port))) return false;
	Server.BindSnapshotRoute([&Service](FHttpResultCallback OnComplete) { Service.Request(MoveTemp(OnComplete)); });

	// Alpha 0, as the primary view's final image delivers it: the PNG must still
	// be opaque, or viewers show a blank/white image.
	TArray<FColor> Pixels;
	Pixels.Init(FColor(10, 200, 30, 0), W * H);
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
	TestEqual(TEXT("opaque even though the frame's alpha was 0"), static_cast<int32>(Raw[3]), 255);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSnapshotShutdownAnswersTest,
	"CamSim.Health.Snapshot.ShutdownAnswersWaitingRequests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSnapshotShutdownAnswersTest::RunTest(const FString& Parameters)
{
	TArray<int32> Codes;
	auto Record = [&Codes](TUniquePtr<FHttpServerResponse>&& R) { Codes.Add(R.IsValid() ? static_cast<int32>(R->Code) : 0); };
	{
		FCamSimSnapshotService Service;
		Service.Request(Record);                          // waiting for a frame
		Service.Request(Record);
		const TArray<FColor> Frame = { FColor::Red };
		Service.OfferFrame(Frame, 1, 1);                  // both now in flight (encoding)
		Service.Request(Record);                          // waiting again
	}
	TestEqual(TEXT("every waiting and in-flight request is answered once"), Codes.Num(), 3);
	for (int32 Code : Codes)
	{
		TestEqual(TEXT("503 on shutdown"), Code, 503);
	}
	return true;
}
