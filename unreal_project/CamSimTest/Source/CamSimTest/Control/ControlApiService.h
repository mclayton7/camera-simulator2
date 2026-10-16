// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpRequestHandler.h"
#include "HttpServerRequest.h"
#include "Config/CamSimConfig.h"
#include "Control/ControlApiTypes.h"
#include "Control/ControlCaptureGate.h"

struct FCamSimHealthServer;
class FControlApiAdapter;
class FSimClock;
struct FControlCaptureJob;
struct FHttpServerRequest;

/**
 * What the control API needs from the world, as functions, so the routes are testable without one. UCamSimSubsystem
 * fills them from the camera and environment actors. Game thread.
 */
struct FControlApiHooks
{
	TFunction<bool()> HasCamera;
	TFunction<void(const FControlCameraCommand&)> ApplyCamera;
	TFunction<void(bool)> SetCameraSuppressed;
	/** GET /api/v1/camera body (a JSON object). */
	TFunction<FString()> CameraJson;
	/** This tick's scene for the capture gate (bCommandThisTick is filled by the service). */
	TFunction<FControlCaptureInputs()> SceneState;
	TFunction<void(TSharedRef<FControlCaptureJob, ESPMode::ThreadSafe>)> RequestCaptureFrame;
	/** False: no environment actor (503). */
	TFunction<bool(const FAtmosphereCommand&)> ApplyAtmosphere;
	TFunction<bool(const FWeatherCommand&)> ApplyWeather;
	TFunction<bool(float Beaufort, float DirectionDeg)> ApplySea;
	/** The entity's committed pose (after clamping); false if it isn't spawned. */
	TFunction<bool(const FEntityKey&, CamSimFrames::FGeoPose&)> EntityPose;
	/** Extra /api/v1/status fields as JSON members without braces ("\"ready\":true,\"frame\":12"). */
	TFunction<FString()> StatusFields;
	/** The clock (null: FSimClock::Get()). */
	FSimClock* Clock = nullptr;
};

/**
 * FControlApiService
 *
 * The control API (ROADMAP 2.4, docs/control-api.md, spec docs/superpowers/specs/2026-10-15-control-api-design.md):
 * /api/v1 routes on the health server. Requests are parsed (ControlApiJson) into canonical commands; entities go to
 * the adapter (polled by the entity manager), the camera and environment through the hooks. A capture runs
 * FControlCaptureGate in Tick(), tags the capture frame, and answers with the filled job. Game thread only (the
 * health server's handlers run on the core ticker).
 */
class CAMSIMTEST_API FControlApiService
{
public:
	FControlApiService(FControlApiAdapter& InAdapter, FControlApiHooks InHooks, const FCamSimConfig::FControlApiConfig& InCfg);
	/** Answers 503 to a capture still in flight, so its callback completes once. */
	~FControlApiService();

	void Bind(FCamSimHealthServer& Server);
	/** Bind through any router-like function (the route tests call the handlers directly). */
	using FAddRoute = TFunctionRef<void(const TCHAR* Path, EHttpServerRequestVerbs Verbs, FHttpRequestHandler Handler)>;
	void BindTo(FAddRoute AddRoute);

	/** Capture gate, delivery and timeouts. From UCamSimSubsystem::Tick, before the camera captures this frame. */
	void Tick(double NowSec);

	bool OwnsCamera() const { return bOwnsCamera; }

	/** Seconds a fired capture may wait for its frame before answering 503. */
	static constexpr double DeliveryTimeoutSec = 10.0;

private:
	bool HandleStatus(const FHttpServerRequest& Req, const FHttpResultCallback& Done);
	bool HandleClock(const FHttpServerRequest& Req, const FHttpResultCallback& Done);
	bool HandleStep(const FHttpServerRequest& Req, const FHttpResultCallback& Done);
	bool HandleCamera(const FHttpServerRequest& Req, const FHttpResultCallback& Done);
	bool HandleEntity(const FHttpServerRequest& Req, const FHttpResultCallback& Done);
	bool HandleEntities(const FHttpServerRequest& Req, const FHttpResultCallback& Done);
	bool HandleEnvironment(const FHttpServerRequest& Req, const FHttpResultCallback& Done);
	bool HandleCapture(const FHttpServerRequest& Req, const FHttpResultCallback& Done);

	FSimClock& Clock() const;
	FString ClockJson() const;
	FString EntitiesJson() const;
	void FinishCapture(int32 Code, const FString& Body);

	FControlApiAdapter& Adapter;
	FControlApiHooks Hooks;
	FCamSimConfig::FControlApiConfig Cfg;
	bool bOwnsCamera = false;

	// The capture in flight (one at a time)
	FControlCaptureGate Gate;
	FHttpResultCallback CaptureDone;
	TSharedPtr<FControlCaptureJob, ESPMode::ThreadSafe> CaptureJob;
	bool   bCaptureWantDepth = true;
	bool   bCaptureWantSemantic = true;
	double CaptureBeginSec = 0.0;
	double CaptureFiredSec = 0.0;
	int32  CaptureFramesWaited = 0;
	uint64 LastCommandSerial = 0;
};
