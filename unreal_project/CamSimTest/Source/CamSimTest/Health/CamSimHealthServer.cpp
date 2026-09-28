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
                                FStatusQueryFn InIsSensorGraphReady,
                                FStatusQueryFn InIsCigiReady,
                                FStatusQueryFn InHasFirstFrame,
                                FStatusQueryFn InIsTerrainReady,
                                TFunction<FString()> InGetPrometheusMetrics)
{
	IsAlive = MoveTemp(InIsAlive);
	IsEncoderReady = MoveTemp(InIsEncoderReady);
	IsSensorGraphReady = MoveTemp(InIsSensorGraphReady);
	IsCigiReady = MoveTemp(InIsCigiReady);
	HasFirstFrame = MoveTemp(InHasFirstFrame);
	IsTerrainReady = MoveTemp(InIsTerrainReady);
	GetPrometheusMetrics = MoveTemp(InGetPrometheusMetrics);
	ListenPort = Port;
	LastTickTimeSec = FPlatformTime::Seconds();
	BindFailedSinceSec = 0.0;

	if (TryListen()) return true;

	// The engine's only reuse option also sets SO_REUSEPORT, which would let a
	// second CamSim share the port silently, so wait for the port instead.
	BindFailedSinceSec = FPlatformTime::Seconds();
	bReportedStillBusy = false;
	UE_LOG(LogCamSim, Warning,
		TEXT("FCamSimHealthServer: port %d is busy (another process, or TIME_WAIT from a run restarted within ~30 s); NOT listening, retrying every %.0f s"),
		Port, BindRetryIntervalSec);
	RetryHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FCamSimHealthServer::RetryListen), BindRetryIntervalSec);
	return false;
}

bool FCamSimHealthServer::RetryListen(float /*DeltaTime*/)
{
	if (TryListen())
	{
		RetryHandle.Reset();
		return false;   // stop ticking
	}
	const double BusySec = FPlatformTime::Seconds() - BindFailedSinceSec;
	if (!bReportedStillBusy && BusySec > 60.0)
	{
		bReportedStillBusy = true;
		UE_LOG(LogCamSim, Error,
			TEXT("FCamSimHealthServer: port %d still busy after %.0f s; is another process using it? Still retrying"),
			ListenPort, BusySec);
	}
	return true;
}

bool FCamSimHealthServer::TryListen()
{
	// With listeners enabled first, GetHttpRouter binds immediately, and
	// bFailOnBindFailure makes a failed bind return null instead of a router
	// that never answers.
	FHttpServerModule::Get().StartAllListeners();
	Router = FHttpServerModule::Get().GetHttpRouter(ListenPort, /*bFailOnBindFailure=*/true);
	if (!Router) return false;

	BindRoutes();

	FString SnapshotSuffix;
	for (const auto& Pair : SnapshotHandlers)
	{
		SnapshotSuffix += TEXT(" ") + Pair.Key;
	}

	if (BindFailedSinceSec > 0.0)
	{
		UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: listening on port %d after %.0f s (/live /health /ready /metrics%s)"),
			ListenPort, FPlatformTime::Seconds() - BindFailedSinceSec, *SnapshotSuffix);
	}
	else
	{
		UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: listening on port %d (/live /health /ready /metrics%s)"),
			ListenPort, *SnapshotSuffix);
	}
	return true;
}

void FCamSimHealthServer::BindRoutes()
{
	// Shared liveness handler — used for both /live (K8s convention) and
	// /health (sim-environment REST orchestrator convention). Both names
	// probe the same watchdog: 200 when Tick() fired within 5s, 503 with
	// stall duration otherwise.
	auto LivenessHandler = FHttpRequestHandler::CreateLambda(
		[this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
		{
			const double AgeSec = FPlatformTime::Seconds() - LastTickTimeSec;
			if (AgeSec < 5.0)
			{
				auto Response = FHttpServerResponse::Create(FString(TEXT("{\"status\":\"ok\"}")), TEXT("application/json"));
				OnComplete(MoveTemp(Response));
			}
			else
			{
				FString Body = FString::Printf(TEXT("{\"status\":\"stalled\",\"last_tick_ago_s\":%.1f}"), AgeSec);
				auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
				Response->Code = EHttpServerResponseCodes::ServiceUnavail;
				OnComplete(MoveTemp(Response));
			}
			return true;
		});

	// GET /live — K8s liveness probe convention
	RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/live")),   EHttpServerRequestVerbs::VERB_GET, LivenessHandler));
	// GET /health — sim-environment REST orchestrator convention (same handler)
	RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/health")), EHttpServerRequestVerbs::VERB_GET, LivenessHandler));

	// GET /ready
	RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/ready")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
		{
			const bool bEncoder = IsEncoderReady ? IsEncoderReady() : false;
			const bool bSensorGraph = IsSensorGraphReady ? IsSensorGraphReady() : false;
			const bool bCigi = IsCigiReady ? IsCigiReady() : false;
			const bool bFrame = HasFirstFrame ? HasFirstFrame() : false;
			const bool bTerrain = IsTerrainReady ? IsTerrainReady() : false;
			const bool bReady = bEncoder && bSensorGraph && bCigi && bFrame && bTerrain;

			FString Body = FString::Printf(
				TEXT("{\"status\":\"%s\",\"encoder\":%s,\"sensor_graph\":%s,\"cigi\":%s,\"first_frame\":%s,\"terrain_ready\":%s}"),
				bReady ? TEXT("ready") : TEXT("not_ready"),
				bEncoder ? TEXT("true") : TEXT("false"),
				bSensorGraph ? TEXT("true") : TEXT("false"),
				bCigi ? TEXT("true") : TEXT("false"),
				bFrame ? TEXT("true") : TEXT("false"),
				bTerrain ? TEXT("true") : TEXT("false"));

			auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
			if (!bReady)
			{
				Response->Code = EHttpServerResponseCodes::ServiceUnavail;
			}
			OnComplete(MoveTemp(Response));
			return true;
		})));

	// GET /metrics — serve the game-thread-cached snapshot so the HTTP handler
	// doesn't run expensive work (entity walks, percentile calcs) on the
	// listener thread while the scraper holds the socket open.
	RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/metrics")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
		{
			TSharedRef<FString, ESPMode::ThreadSafe> Snapshot = [this]()
			{
				FScopeLock Lock(&MetricsLock_);
				return CachedMetrics_;
			}();
			auto Response = FHttpServerResponse::Create(*Snapshot, TEXT("text/plain; charset=utf-8"));
			OnComplete(MoveTemp(Response));
			return true;
		})));

	for (const auto& Pair : SnapshotHandlers)
	{
		BindSnapshotHandler(Pair.Key);
	}
}

void FCamSimHealthServer::BindSnapshotHandler(const FString& Path)
{
	RouteHandles.Add(Router->BindRoute(FHttpPath(Path), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this, Path](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			SnapshotHandlers[Path](OnComplete);
			return true;
		})));
}

void FCamSimHealthServer::BindSnapshotRoute(const FString& Path, TFunction<void(FHttpResultCallback)> Handler)
{
	const bool bRebind = SnapshotHandlers.Contains(Path);
	SnapshotHandlers.Add(Path, MoveTemp(Handler));
	if (!Router || bRebind) return;   // bound with the other routes once listening
	BindSnapshotHandler(Path);
	UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: %s enabled on port %d"), *Path, ListenPort);
}

void FCamSimHealthServer::Stop()
{
	if (RetryHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RetryHandle);
		RetryHandle.Reset();
	}
	if (Router)
	{
		// Unbind so a later Start() on the same port starts from a clean router
		// (FHttpServerModule keeps one router per port for the process lifetime).
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			if (Handle) Router->UnbindRoute(Handle);
		}
		RouteHandles.Reset();
		FHttpServerModule::Get().StopAllListeners();
		Router.Reset();
		UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: stopped"));
	}
}

void FCamSimHealthServer::UpdateTick()
{
	LastTickTimeSec = FPlatformTime::Seconds();
}

void FCamSimHealthServer::UpdateMetricsSnapshot()
{
	// Build the body on the caller's thread (expected to be the game thread,
	// where Prometheus counter sources are free of synchronisation). Publish
	// it atomically for the HTTP handler to read.
	if (!GetPrometheusMetrics) return;
	TSharedRef<FString, ESPMode::ThreadSafe> NewBody =
		MakeShared<FString, ESPMode::ThreadSafe>(GetPrometheusMetrics());
	FScopeLock Lock(&MetricsLock_);
	CachedMetrics_ = NewBody;
}
