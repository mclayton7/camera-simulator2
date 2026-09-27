// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"
#include "Containers/Ticker.h"

class IHttpRouter;

/**
 * FCamSimHealthServer
 *
 * Lightweight HTTP server for Kubernetes liveness/readiness probes.
 * Uses UE5's FHttpServerModule (IHttpRouter).
 *
 * Routes:
 *   GET /live    -- 200 if game loop ticked within 5s
 *   GET /ready   -- 200 if encoder + CIGI + first frame + terrain all OK
 *   GET /metrics -- Prometheus exposition format
 *   GET /snapshot        -- next grabbed frame as PNG (only after BindSnapshotRoute)
 *   GET /snapshot/sensor -- next grabbed sensor frame as PNG (only after BindSnapshotRoute)
 */
struct FCamSimHealthServer
{
	using FStatusQueryFn = TFunction<bool()>;

	/** Seconds between bind attempts while the port is busy. */
	static constexpr float BindRetryIntervalSec = 2.0f;

	~FCamSimHealthServer() { Stop(); }

	/**
	 * Start the HTTP server on the given port. Returns true when it is
	 * listening. If the port is busy (another process, or TIME_WAIT from a
	 * run restarted within ~30 s) it returns false, logs it, and retries on
	 * the core ticker every BindRetryIntervalSec until the bind succeeds.
	 */
	bool Start(int32 Port,
	           FStatusQueryFn InIsAlive,
	           FStatusQueryFn InIsEncoderReady,
	           FStatusQueryFn InIsCigiReady,
	           FStatusQueryFn InHasFirstFrame,
	           FStatusQueryFn InIsTerrainReady,
	           TFunction<FString()> InGetPrometheusMetrics);

	/**
	 * Bind GET <Path> to a snapshot handler (ROADMAP 3A/3B). Unbound, the
	 * router answers 404 for that path. The handler owns the callback and
	 * must complete it exactly once. Multiple distinct paths may be bound
	 * (e.g. "/snapshot", "/snapshot/sensor").
	 */
	void BindSnapshotRoute(const FString& Path, TFunction<void(FHttpResultCallback)> Handler);

	/** Stop the HTTP server (and any pending bind retry). */
	void Stop();

	/** True once the port is bound and the routes answer. */
	bool IsListening() const { return Router.IsValid(); }

	/** Call from game thread tick to update the liveness timestamp. */
	void UpdateTick();

	/**
	 * Refresh the cached /metrics body on the game thread. Intended to be
	 * called at a low rate (~1 Hz) from the subsystem tick so HTTP handlers
	 * can serve the snapshot lock-free instead of executing the potentially
	 * expensive build callback inline on the HTTP thread.
	 */
	void UpdateMetricsSnapshot();

private:
	/** Bind the port and the routes. False if the port is busy. */
	bool TryListen();
	void BindRoutes();
	void BindSnapshotHandler(const FString& Path);
	bool RetryListen(float DeltaTime);

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
	double LastTickTimeSec = 0.0;
	int32 ListenPort = 0;
	TMap<FString, TFunction<void(FHttpResultCallback)>> SnapshotHandlers;   // path -> handler, bound once listening
	FTSTicker::FDelegateHandle RetryHandle;
	double BindFailedSinceSec = 0.0;
	bool bReportedStillBusy = false;

	FStatusQueryFn IsAlive;
	FStatusQueryFn IsEncoderReady;
	FStatusQueryFn IsCigiReady;
	FStatusQueryFn HasFirstFrame;
	FStatusQueryFn IsTerrainReady;
	TFunction<FString()> GetPrometheusMetrics;

	/**
	 * Cached /metrics body. Game thread stores a new snapshot via
	 * UpdateMetricsSnapshot(); HTTP threads acquire the same shared ref to
	 * produce the response body.
	 *
	 * Lock rationale: `ESPMode::ThreadSafe` makes the CONTROL BLOCK refcount
	 * atomic, but reassigning a TSharedRef member (object pointer + control
	 * pointer) is still two unsynchronised stores. Without the lock an HTTP
	 * thread could read a new object pointer paired with the old control
	 * block (or vice versa) while the game thread is mid-swap. The critical
	 * section below holds only long enough to copy the TSharedRef by value
	 * — the FString body is then used outside the lock.
	 */
	TSharedRef<FString, ESPMode::ThreadSafe> CachedMetrics_ =
		MakeShared<FString, ESPMode::ThreadSafe>();
	mutable FCriticalSection MetricsLock_;
};
