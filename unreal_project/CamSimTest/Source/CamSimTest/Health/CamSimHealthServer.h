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
 *   GET /live    -- 200 if game loop ticked within 5s
 *   GET /ready   -- 200 if encoder + CIGI + first frame all OK
 *   GET /metrics -- Prometheus exposition format
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
