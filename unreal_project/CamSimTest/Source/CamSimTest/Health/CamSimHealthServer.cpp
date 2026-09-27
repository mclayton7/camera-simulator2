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
                                FStatusQueryFn InIsTerrainReady,
                                TFunction<FString()> InGetPrometheusMetrics)
{
	IsAlive = MoveTemp(InIsAlive);
	IsEncoderReady = MoveTemp(InIsEncoderReady);
	IsCigiReady = MoveTemp(InIsCigiReady);
	HasFirstFrame = MoveTemp(InHasFirstFrame);
	IsTerrainReady = MoveTemp(InIsTerrainReady);
	GetPrometheusMetrics = MoveTemp(InGetPrometheusMetrics);
	ListenPort = Port;
	LastTickTimeSec = FPlatformTime::Seconds();

	Router = FHttpServerModule::Get().GetHttpRouter(Port);
	if (!Router)
	{
		UE_LOG(LogCamSim, Error, TEXT("FCamSimHealthServer: failed to get HTTP router on port %d"), Port);
		return false;
	}

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
			const bool bCigi = IsCigiReady ? IsCigiReady() : false;
			const bool bFrame = HasFirstFrame ? HasFirstFrame() : false;
			const bool bTerrain = IsTerrainReady ? IsTerrainReady() : false;
			const bool bReady = bEncoder && bCigi && bFrame && bTerrain;

			FString Body = FString::Printf(
				TEXT("{\"status\":\"%s\",\"encoder\":%s,\"cigi\":%s,\"first_frame\":%s,\"terrain_ready\":%s}"),
				bReady ? TEXT("ready") : TEXT("not_ready"),
				bEncoder ? TEXT("true") : TEXT("false"),
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

	FHttpServerModule::Get().StartAllListeners();
	UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: listening on port %d (/live /health /ready /metrics)"), Port);
	return true;
}

void FCamSimHealthServer::BindSnapshotRoute(TFunction<void(FHttpResultCallback)> Handler)
{
	if (!Router) return;
	RouteHandles.Add(Router->BindRoute(FHttpPath(TEXT("/snapshot")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([Handler = MoveTemp(Handler)](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			Handler(OnComplete);
			return true;
		})));
	UE_LOG(LogCamSim, Log, TEXT("FCamSimHealthServer: /snapshot enabled on port %d"), ListenPort);
}

void FCamSimHealthServer::Stop()
{
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
