// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Control/ControlApiAdapter.h"
#include "Control/ControlApiService.h"
#include "Control/ControlCaptureJob.h"
#include "Health/CamSimHealthServer.h"
#include "Time/SimClock.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"

// -------------------------------------------------------------------------
// /api/v1 routes over real HTTP (ROADMAP 2.4). The world is faked through
// FControlApiHooks; both tickers are pumped (CLAUDE.md: HTTP automation tests
// need two tickers), and the service is ticked as the subsystem would.
// -------------------------------------------------------------------------

namespace
{
	struct FHttpResult
	{
		int32 Code = 0;
		FString Body;
		TSharedPtr<FJsonObject> Json() const
		{
			TSharedPtr<FJsonObject> O;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), O);
			return O;
		}
	};

	/** A fake world: a camera that may or may not exist, scene state, and a capture hook that keeps the job. */
	struct FFakeWorld
	{
		bool bHasCamera = true;
		int32 CameraApplies = 0;
		bool bSuppressed = false;
		FControlCaptureInputs Scene;
		TSharedPtr<FControlCaptureJob, ESPMode::ThreadSafe> Job;
		int32 AtmosphereApplies = 0;
		float SeaBeaufort = -1.f;
		FSimClock Clock;
		double Mono = 0.0;

		FFakeWorld()
		{
			Clock.SetMonotonicSource([this] { return Mono; });
			Clock.SetUtc(FDateTime(2026, 7, 1, 12, 0, 0));
			Scene.bTerrainReady = true;
			Scene.MinLoadProgressPct = 100.f;
		}

		FControlApiHooks Hooks()
		{
			FControlApiHooks H;
			H.HasCamera = [this] { return bHasCamera; };
			H.ApplyCamera = [this](const FControlCameraCommand&) { ++CameraApplies; };
			H.SetCameraSuppressed = [this](bool b) { bSuppressed = b; };
			H.CameraJson = [] { return FString(TEXT("{\"lat\":1}")); };
			H.SceneState = [this] { return Scene; };
			H.RequestCaptureFrame = [this](TSharedRef<FControlCaptureJob, ESPMode::ThreadSafe> J) { Job = J; };
			H.ApplyAtmosphere = [this](const FAtmosphereCommand&) { ++AtmosphereApplies; return true; };
			H.ApplyWeather = [](const FWeatherCommand&) { return true; };
			H.ApplySea = [this](float B, float) { SeaBeaufort = B; return true; };
			H.EntityPose = [](const FEntityKey&, CamSimFrames::FGeoPose&) { return false; };
			H.StatusFields = [] { return FString(TEXT("\"ready\":false")); };
			H.Clock = &Clock;
			return H;
		}
	};

	/**
	 * The service's routes, called directly (no sockets: UE keeps one listener per port for the process and restarts
	 * them all on every Start(), so many short-lived servers collide in TIME_WAIT). Matching follows UE's router: an
	 * exact path, else a pattern with ":id" segments. ReadyCountsApiOwnership goes over real HTTP.
	 */
	struct FHarness
	{
		FFakeWorld World;
		FControlApiAdapter Adapter;
		FCamSimConfig::FControlApiConfig Cfg;
		TUniquePtr<FControlApiService> Service;
		struct FRoute { FString Path; EHttpServerRequestVerbs Verbs; FHttpRequestHandler Handler; };
		TArray<FRoute> Routes;
		bool bStarted = true;

		FHarness()
		{
			Cfg.CaptureTimeoutSec = 30.f;
			Service = MakeUnique<FControlApiService>(Adapter, World.Hooks(), Cfg);
			Service->BindTo([this](const TCHAR* Path, EHttpServerRequestVerbs Verbs, FHttpRequestHandler Handler)
			{
				Routes.Add({ Path, Verbs, MoveTemp(Handler) });
			});
		}

		void Pump()
		{
			World.Scene.FramesCaptured++;
			Service->Tick(FPlatformTime::Seconds());
			FPlatformProcess::Sleep(0.002f);
		}

		static EHttpServerRequestVerbs VerbOf(const FString& V)
		{
			if (V == TEXT("GET")) return EHttpServerRequestVerbs::VERB_GET;
			if (V == TEXT("PUT")) return EHttpServerRequestVerbs::VERB_PUT;
			if (V == TEXT("POST")) return EHttpServerRequestVerbs::VERB_POST;
			if (V == TEXT("DELETE")) return EHttpServerRequestVerbs::VERB_DELETE;
			return EHttpServerRequestVerbs::VERB_NONE;
		}

		const FRoute* Match(const FString& Path, TMap<FString, FString>& OutParams) const
		{
			for (const FRoute& R : Routes) if (R.Path == Path) return &R;
			TArray<FString> Want, Have;
			Path.ParseIntoArray(Have, TEXT("/"));
			for (const FRoute& R : Routes)
			{
				R.Path.ParseIntoArray(Want, TEXT("/"));
				if (Want.Num() != Have.Num()) continue;
				TMap<FString, FString> Params;
				bool bOk = true;
				for (int32 I = 0; I < Want.Num() && bOk; ++I)
				{
					if (Want[I].StartsWith(TEXT(":"))) Params.Add(Want[I].Mid(1), Have[I]);
					else bOk = Want[I] == Have[I];
				}
				if (bOk) { OutParams = Params; return &R; }
			}
			return nullptr;
		}

		/** Call and tick the service until answered; OnPump runs every tick (e.g. to complete a capture job). */
		FHttpResult Call(const FString& Verb, const FString& Path, const FString& Body = FString(),
			TFunction<void(FHarness&)> OnPump = nullptr, double TimeoutSec = 8.0)
		{
			FHttpResult Out;
			FHttpServerRequest Req;
			Req.Verb = VerbOf(Verb);
			const FRoute* R = Match(Path, Req.PathParams);
			if (!R || !EnumHasAnyFlags(R->Verbs, Req.Verb)) { Out.Code = 404; return Out; }
			const FTCHARToUTF8 Utf8(*Body);
			Req.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			bool bDone = false;
			FHttpResultCallback Done = [&bDone, &Out](TUniquePtr<FHttpServerResponse> Resp)
			{
				bDone = true;
				Out.Code = static_cast<int32>(Resp->Code);
				Out.Body = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Resp->Body.GetData()), Resp->Body.Num()));
			};
			R->Handler.Execute(Req, Done);
			const double Deadline = FPlatformTime::Seconds() + TimeoutSec;
			while (!bDone && FPlatformTime::Seconds() < Deadline)
			{
				Pump();
				if (OnPump) OnPump(*this);
			}
			return Out;
		}
	};
}

#define ROUTES_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "CamSim.ControlApi.Routes." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

ROUTES_TEST(FRoutesStatusTest, "StatusShape")
bool FRoutesStatusTest::RunTest(const FString& Parameters)
{
	FHarness H;
	if (!TestTrue(TEXT("server started"), H.bStarted)) return false;
	const FHttpResult R = H.Call(TEXT("GET"), TEXT("/api/v1/status"));
	TestEqual(TEXT("200"), R.Code, 200);
	const TSharedPtr<FJsonObject> J = R.Json();
	if (!TestTrue(TEXT("json"), J.IsValid())) return false;
	for (const TCHAR* Key : { TEXT("ready"), TEXT("camera_owned"), TEXT("lockstep"), TEXT("utc"), TEXT("entities") })
	{
		TestTrue(FString::Printf(TEXT("has %s"), Key), J->HasField(Key));
	}
	TestEqual(TEXT("utc from the clock"), J->GetStringField(TEXT("utc")), FString(TEXT("2026-07-01T12:00:00.000Z")));
	return true;
}

ROUTES_TEST(FRoutesClockTest, "ClockLockstepAndStep")
bool FRoutesClockTest::RunTest(const FString& Parameters)
{
	FHarness H;
	if (!TestTrue(TEXT("server started"), H.bStarted)) return false;
	TestEqual(TEXT("step outside lockstep: 409"), H.Call(TEXT("POST"), TEXT("/api/v1/clock/step"), TEXT(R"({"seconds":1})")).Code, 409);
	TestEqual(TEXT("lockstep on"), H.Call(TEXT("PUT"), TEXT("/api/v1/clock"), TEXT(R"({"lockstep":true})")).Code, 200);
	TestTrue(TEXT("clock in lockstep"), H.World.Clock.IsLockstep());
	const FHttpResult S = H.Call(TEXT("POST"), TEXT("/api/v1/clock/step"), TEXT(R"({"seconds":10})"));
	TestEqual(TEXT("step 200"), S.Code, 200);
	TestEqual(TEXT("clock +10 s"), H.World.Clock.NowUtc(), FDateTime(2026, 7, 1, 12, 0, 10));
	TestTrue(TEXT("returns utc"), S.Body.Contains(TEXT("2026-07-01T12:00:10.000Z")));
	TestEqual(TEXT("rate in lockstep: 409"), H.Call(TEXT("PUT"), TEXT("/api/v1/clock"), TEXT(R"({"rate":2})")).Code, 409);
	TestEqual(TEXT("set utc"), H.Call(TEXT("PUT"), TEXT("/api/v1/clock"), TEXT(R"({"utc":"2026-01-02T03:04:05Z"})")).Code, 200);
	TestEqual(TEXT("utc applied"), H.World.Clock.NowUtc(), FDateTime(2026, 1, 2, 3, 4, 5));
	TestEqual(TEXT("bad step: 400"), H.Call(TEXT("POST"), TEXT("/api/v1/clock/step"), TEXT(R"({"seconds":0})")).Code, 400);
	return true;
}

ROUTES_TEST(FRoutesEntityTest, "EntityCrud")
bool FRoutesEntityTest::RunTest(const FString& Parameters)
{
	FHarness H;
	if (!TestTrue(TEXT("server started"), H.bStarted)) return false;
	TestEqual(TEXT("put"), H.Call(TEXT("PUT"), TEXT("/api/v1/entities/7"), TEXT(R"({"type_id":2001,"lat":33,"lon":-117,"alt":40})")).Code, 200);
	TestEqual(TEXT("new without type: 400"), H.Call(TEXT("PUT"), TEXT("/api/v1/entities/8"), TEXT(R"({"lat":33,"lon":-117,"alt":40})")).Code, 400);
	const FHttpResult L = H.Call(TEXT("GET"), TEXT("/api/v1/entities"));
	TestEqual(TEXT("list 200"), L.Code, 200);
	TestTrue(TEXT("lists 7"), L.Body.Contains(TEXT("\"id\":7")));
	TestEqual(TEXT("adapter has it"), H.Adapter.GetIds().Num(), 1);
	TestEqual(TEXT("delete"), H.Call(TEXT("DELETE"), TEXT("/api/v1/entities/7")).Code, 200);
	TestEqual(TEXT("delete again: 404"), H.Call(TEXT("DELETE"), TEXT("/api/v1/entities/7")).Code, 404);
	TestEqual(TEXT("id 0: 400"), H.Call(TEXT("PUT"), TEXT("/api/v1/entities/0"), TEXT("{}")).Code, 400);
	TestEqual(TEXT("id not a number: 400"), H.Call(TEXT("PUT"), TEXT("/api/v1/entities/abc"), TEXT("{}")).Code, 400);
	H.Call(TEXT("PUT"), TEXT("/api/v1/entities/9"), TEXT(R"({"type_id":2001,"lat":33,"lon":-117,"alt":40})"));
	TestEqual(TEXT("delete all"), H.Call(TEXT("DELETE"), TEXT("/api/v1/entities")).Code, 200);
	TestEqual(TEXT("none left"), H.Adapter.GetIds().Num(), 0);
	return true;
}

ROUTES_TEST(FRoutesCameraTest, "CameraOwnership")
bool FRoutesCameraTest::RunTest(const FString& Parameters)
{
	FHarness H;
	if (!TestTrue(TEXT("server started"), H.bStarted)) return false;
	H.World.bHasCamera = false;
	TestEqual(TEXT("no camera: 503"), H.Call(TEXT("PUT"), TEXT("/api/v1/camera"), TEXT(R"({"hfov_deg":20})")).Code, 503);
	TestFalse(TEXT("not owned"), H.Service->OwnsCamera());
	H.World.bHasCamera = true;
	TestEqual(TEXT("bad body: 400"), H.Call(TEXT("PUT"), TEXT("/api/v1/camera"), TEXT(R"({"lat":1})")).Code, 400);
	TestEqual(TEXT("nothing applied on 400"), H.World.CameraApplies, 0);
	TestEqual(TEXT("put"), H.Call(TEXT("PUT"), TEXT("/api/v1/camera"), TEXT(R"({"hfov_deg":20})")).Code, 200);
	TestTrue(TEXT("owned"), H.Service->OwnsCamera());
	TestTrue(TEXT("CIGI camera suppressed"), H.World.bSuppressed);
	TestEqual(TEXT("applied once"), H.World.CameraApplies, 1);
	TestEqual(TEXT("get"), H.Call(TEXT("GET"), TEXT("/api/v1/camera")).Code, 200);
	TestEqual(TEXT("release"), H.Call(TEXT("DELETE"), TEXT("/api/v1/camera")).Code, 200);
	TestFalse(TEXT("released"), H.Service->OwnsCamera());
	TestFalse(TEXT("CIGI camera back"), H.World.bSuppressed);
	return true;
}

ROUTES_TEST(FRoutesEnvironmentTest, "Environment")
bool FRoutesEnvironmentTest::RunTest(const FString& Parameters)
{
	FHarness H;
	if (!TestTrue(TEXT("server started"), H.bStarted)) return false;
	TestEqual(TEXT("put"), H.Call(TEXT("PUT"), TEXT("/api/v1/environment"),
		TEXT(R"({"atmosphere":{"visibility_m":20000},"sea":{"beaufort":4}})")).Code, 200);
	TestEqual(TEXT("atmosphere applied"), H.World.AtmosphereApplies, 1);
	TestEqual(TEXT("sea applied"), H.World.SeaBeaufort, 4.0f);
	TestEqual(TEXT("bad: 400"), H.Call(TEXT("PUT"), TEXT("/api/v1/environment"), TEXT(R"({"sea":{"beaufort":20}})")).Code, 400);
	return true;
}

ROUTES_TEST(FRoutesCaptureTest, "CaptureRoundTrip")
bool FRoutesCaptureTest::RunTest(const FString& Parameters)
{
	FHarness H;
	if (!TestTrue(TEXT("server started"), H.bStarted)) return false;
	H.World.bHasCamera = false;
	TestEqual(TEXT("no camera: 503"), H.Call(TEXT("POST"), TEXT("/api/v1/capture"), TEXT("{}")).Code, 503);
	H.World.bHasCamera = true;

	int32 SecondCode = 0;
	bool bSecondSent = false;
	const FHttpResult R = H.Call(TEXT("POST"), TEXT("/api/v1/capture"), TEXT(R"({"settle_frames":3})"),
		[&](FHarness& Hn)
		{
			if (Hn.World.Job && !bSecondSent)
			{
				bSecondSent = true;
				// A second capture while one is in flight. (Answered synchronously, inside this pump.)
				SecondCode = Hn.Call(TEXT("POST"), TEXT("/api/v1/capture"), TEXT("{}")).Code;
				FControlCaptureJob& J = *Hn.World.Job;
				J.FrameIdx = 77; J.TimestampUs = 5; J.Width = 4; J.Height = 2;
				J.Record = TEXT("{\"frame_id\":77,\"annotations\":[]}");
				J.Png = { 1, 2, 3 };
				J.DepthPng = { 4, 5 };
				J.DepthFarPlaneM = 5000.f;
				J.bDone.Store(true, EMemoryOrder::SequentiallyConsistent);
			}
		});
	TestEqual(TEXT("second capture: 409"), SecondCode, 409);
	TestEqual(TEXT("capture 200"), R.Code, 200);
	const TSharedPtr<FJsonObject> J = R.Json();
	if (!TestTrue(TEXT("json"), J.IsValid())) { AddInfo(R.Body.Left(300)); return false; }
	TestEqual(TEXT("frame id"), J->GetIntegerField(TEXT("frame_id")), 77);
	TestEqual(TEXT("png b64"), J->GetStringField(TEXT("image_png_b64")), FBase64::Encode(TArray<uint8>{ 1, 2, 3 }));
	TestEqual(TEXT("depth b64"), J->GetStringField(TEXT("depth_png_b64")), FBase64::Encode(TArray<uint8>{ 4, 5 }));
	const TSharedPtr<FJsonObject>* Ann = nullptr;
	TestTrue(TEXT("annotation is the record"), J->TryGetObjectField(TEXT("annotation"), Ann) && (*Ann)->GetIntegerField(TEXT("frame_id")) == 77);
	const TSharedPtr<FJsonObject>* Settle = nullptr;
	TestTrue(TEXT("settle info"), J->TryGetObjectField(TEXT("settle"), Settle) && (*Settle)->GetIntegerField(TEXT("frames_waited")) >= 3);
	return true;
}

ROUTES_TEST(FRoutesCaptureTimeoutTest, "CaptureTimesOut")
bool FRoutesCaptureTimeoutTest::RunTest(const FString& Parameters)
{
	FHarness H;
	if (!TestTrue(TEXT("server started"), H.bStarted)) return false;
	H.World.Scene.bTerrainReady = false;
	const FHttpResult R = H.Call(TEXT("POST"), TEXT("/api/v1/capture"), TEXT(R"({"timeout_s":1})"));
	TestEqual(TEXT("503"), R.Code, 503);
	TestTrue(TEXT("says what it waited for"), R.Body.Contains(TEXT("terrain")));
	H.World.Scene.bTerrainReady = true;
	TestFalse(TEXT("no job was requested"), H.World.Job.IsValid());
	return true;
}

ROUTES_TEST(FRoutesBadJsonTest, "BadJson400")
bool FRoutesBadJsonTest::RunTest(const FString& Parameters)
{
	FHarness H;
	if (!TestTrue(TEXT("server started"), H.bStarted)) return false;
	const FHttpResult R = H.Call(TEXT("PUT"), TEXT("/api/v1/clock"), TEXT("{nope"));
	TestEqual(TEXT("400"), R.Code, 400);
	TestTrue(TEXT("error json"), R.Json().IsValid() && R.Json()->HasField(TEXT("error")));
	return true;
}

ROUTES_TEST(FRoutesReadyTest, "ReadyCountsApiOwnership")
bool FRoutesReadyTest::RunTest(const FString& Parameters)
{
	// Real HTTP on a port of its own, with listener address reuse as SnapshotEndpointTest sets it: UE restarts every
	// registered listener on each Start(), and a listener created without reuse can't rebind from TIME_WAIT, which
	// fails the health server tests that run later.
	GConfig->SetBool(TEXT("HTTPServer.Listeners"), TEXT("DefaultReuseAddressAndPort"), true, GEngineIni);
	FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("HTTPServer.Listeners") });
	constexpr int32 Port = 48083;
	FHarness H;
	FCamSimHealthServer Server;
	const bool bStarted = Server.Start(Port, [] { return true; }, [] { return true; }, [] { return true; },
		[&H] { return H.Service->OwnsCamera(); }, [] { return true; }, [] { return true; }, [] { return FString(); });
	if (!TestTrue(TEXT("server started"), bStarted)) return false;
	H.Service->Bind(Server);

	auto Get = [&](const FString& Verb, const FString& Path, const FString& Body) -> FHttpResult
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> R = FHttpModule::Get().CreateRequest();
		R->SetVerb(Verb);
		R->SetURL(FString::Printf(TEXT("http://127.0.0.1:%d%s"), Port, *Path));
		R->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		if (!Body.IsEmpty()) R->SetContentAsString(Body);
		R->SetTimeout(5.0);
		bool bDone = false;
		FHttpResult Out;
		R->OnProcessRequestComplete().BindLambda([&](FHttpRequestPtr, FHttpResponsePtr Resp, bool)
		{
			bDone = true;
			if (Resp) { Out.Code = Resp->GetResponseCode(); Out.Body = Resp->GetContentAsString(); }
		});
		R->ProcessRequest();
		const double Deadline = FPlatformTime::Seconds() + 6.0;
		while (!bDone && FPlatformTime::Seconds() < Deadline)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.01f);
			FTSTicker::GetCoreTicker().Tick(0.01f);
			FPlatformProcess::Sleep(0.01f);
		}
		return Out;
	};

	TestEqual(TEXT("not ready without CIGI or the API"), Get(TEXT("GET"), TEXT("/ready"), FString()).Code, 503);
	const FHttpResult Put = Get(TEXT("PUT"), TEXT("/api/v1/camera"), TEXT(R"({"hfov_deg":20})"));
	TestEqual(TEXT("camera over HTTP"), Put.Code, 200);
	TestEqual(TEXT("ready once the API owns the camera"), Get(TEXT("GET"), TEXT("/ready"), FString()).Code, 200);
	TestEqual(TEXT("status over HTTP"), Get(TEXT("GET"), TEXT("/api/v1/status"), FString()).Code, 200);
	TestEqual(TEXT("entity path param over HTTP"), Get(TEXT("PUT"), TEXT("/api/v1/entities/12"),
		TEXT(R"({"type_id":2001,"lat":1,"lon":2,"alt":3})")).Code, 200);
	TestEqual(TEXT("adapter got entity 12"), H.Adapter.GetTypeId(12), static_cast<uint16>(2001));
	Server.Stop();
	return true;
}

#undef ROUTES_TEST
