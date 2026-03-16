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
