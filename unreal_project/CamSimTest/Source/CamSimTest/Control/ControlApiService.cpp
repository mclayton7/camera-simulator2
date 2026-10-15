// Copyright CamSim Contributors. All Rights Reserved.

#include "Control/ControlApiService.h"
#include "CamSimTest.h"
#include "Control/ControlApiAdapter.h"
#include "Control/ControlApiJson.h"
#include "Control/ControlCaptureJob.h"
#include "Geospatial/CigiFrames.h"
#include "Health/CamSimHealthServer.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/Base64.h"
#include "Time/SimClock.h"

namespace
{
	using namespace CamSimControl;

	FString BodyText(const FHttpServerRequest& Req)
	{
		if (Req.Body.IsEmpty()) return FString();
		const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Req.Body.GetData()), Req.Body.Num());
		return FString(Conv.Length(), Conv.Get());
	}

	void Respond(const FHttpResultCallback& Done, int32 Code, const FString& Json)
	{
		TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(Json, TEXT("application/json"));
		R->Code = static_cast<EHttpServerResponseCodes>(Code);
		Done(MoveTemp(R));
	}

	void RespondError(const FHttpResultCallback& Done, int32 Code, const FString& Message)
	{
		Respond(Done, Code, ErrorJson(Message));
	}

	const TCHAR* Bool(bool b) { return b ? TEXT("true") : TEXT("false"); }

	/** Path parameter "id" as an entity id; 0 when it isn't a number (ParseEntity rejects 0). */
	uint32 PathId(const FHttpServerRequest& Req)
	{
		const FString* S = Req.PathParams.Find(TEXT("id"));
		if (!S || S->IsEmpty() || S->Len() > 6 || !S->IsNumeric() || S->Contains(TEXT(".")) || S->Contains(TEXT("-"))) return 0;
		return static_cast<uint32>(FCString::Atoi(**S));
	}
}

FControlApiService::FControlApiService(FControlApiAdapter& InAdapter, FControlApiHooks InHooks,
	const FCamSimConfig::FControlApiConfig& InCfg)
	: Adapter(InAdapter), Hooks(MoveTemp(InHooks)), Cfg(InCfg)
{
	LastCommandSerial = Adapter.GetCommandSerial();
}

FControlApiService::~FControlApiService()
{
	if (CaptureDone) FinishCapture(503, ErrorJson(TEXT("CamSim is shutting down")));
}

FSimClock& FControlApiService::Clock() const
{
	return Hooks.Clock ? *Hooks.Clock : FSimClock::Get();
}

void FControlApiService::Bind(FCamSimHealthServer& Server)
{
	BindTo([&Server](const TCHAR* Path, EHttpServerRequestVerbs Verbs, FHttpRequestHandler Handler)
	{
		Server.BindRoute(Path, Verbs, MoveTemp(Handler));
	});
	UE_LOG(LogCamSim, Log, TEXT("FControlApiService: /api/v1 routes bound (control_api.enabled)"));
}

void FControlApiService::BindTo(FAddRoute AddRoute)
{
	using V = EHttpServerRequestVerbs;
	auto H = [this](bool (FControlApiService::*Fn)(const FHttpServerRequest&, const FHttpResultCallback&))
	{
		return FHttpRequestHandler::CreateLambda([this, Fn](const FHttpServerRequest& Req, const FHttpResultCallback& Done)
		{
			return (this->*Fn)(Req, Done);
		});
	};
	AddRoute(TEXT("/api/v1/status"), V::VERB_GET, H(&FControlApiService::HandleStatus));
	AddRoute(TEXT("/api/v1/clock"), V::VERB_GET | V::VERB_PUT, H(&FControlApiService::HandleClock));
	AddRoute(TEXT("/api/v1/clock/step"), V::VERB_POST, H(&FControlApiService::HandleStep));
	AddRoute(TEXT("/api/v1/camera"), V::VERB_GET | V::VERB_PUT | V::VERB_DELETE, H(&FControlApiService::HandleCamera));
	AddRoute(TEXT("/api/v1/entities"), V::VERB_GET | V::VERB_DELETE, H(&FControlApiService::HandleEntities));
	AddRoute(TEXT("/api/v1/entities/:id"), V::VERB_PUT | V::VERB_DELETE, H(&FControlApiService::HandleEntity));
	AddRoute(TEXT("/api/v1/environment"), V::VERB_PUT, H(&FControlApiService::HandleEnvironment));
	AddRoute(TEXT("/api/v1/capture"), V::VERB_POST, H(&FControlApiService::HandleCapture));
}

// -------------------------------------------------------------------------
// Status and clock
// -------------------------------------------------------------------------

FString FControlApiService::ClockJson() const
{
	const FSimClock& C = Clock();
	return FString::Printf(TEXT("{\"utc\":\"%s\",\"lockstep\":%s,\"rate\":%g}"),
		*UtcIso(C.NowUtc()), Bool(C.IsLockstep()), C.GetRate());
}

bool FControlApiService::HandleStatus(const FHttpServerRequest& Req, const FHttpResultCallback& Done)
{
	const FString Extra = Hooks.StatusFields ? Hooks.StatusFields() : FString();
	Respond(Done, 200, FString::Printf(TEXT("{%s%s\"camera_owned\":%s,\"lockstep\":%s,\"utc\":\"%s\",\"entities\":%d,\"capture_pending\":%s}"),
		*Extra, Extra.IsEmpty() ? TEXT("") : TEXT(","), Bool(bOwnsCamera), Bool(Clock().IsLockstep()),
		*UtcIso(Clock().NowUtc()), Adapter.GetIds().Num(), Bool(CaptureDone != nullptr)));
	return true;
}

bool FControlApiService::HandleClock(const FHttpServerRequest& Req, const FHttpResultCallback& Done)
{
	if (Req.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		Respond(Done, 200, ClockJson());
		return true;
	}
	const auto P = ParseClock(BodyText(Req));
	if (P.HasError())
	{
		RespondError(Done, 400, P.GetError().Message);
		return true;
	}
	const FClockRequest& C = P.GetValue();
	const bool bLockstepAfter = C.bLockstep.Get(Clock().IsLockstep());
	if (C.Rate && bLockstepAfter)
	{
		RespondError(Done, 409, TEXT("rate applies in real-time mode only; the clock is in lockstep (step it instead)"));
		return true;
	}
	if (C.bLockstep) Clock().SetLockstep(*C.bLockstep);
	if (C.Utc) Clock().SetUtc(*C.Utc);
	if (C.Rate) Clock().SetRate(*C.Rate);
	Adapter.NoteCommand(GFrameCounter);   // the sun, sea and thermal move: a capture settles again
	Respond(Done, 200, ClockJson());
	return true;
}

bool FControlApiService::HandleStep(const FHttpServerRequest& Req, const FHttpResultCallback& Done)
{
	const auto P = ParseStep(BodyText(Req));
	if (P.HasError())
	{
		RespondError(Done, 400, P.GetError().Message);
		return true;
	}
	if (!Clock().IsLockstep())
	{
		RespondError(Done, 409, TEXT("the clock is not in lockstep (PUT /api/v1/clock {\"lockstep\":true})"));
		return true;
	}
	Clock().Step(P.GetValue().Seconds);
	Adapter.NoteCommand(GFrameCounter);
	Respond(Done, 200, ClockJson());
	return true;
}

// -------------------------------------------------------------------------
// Camera
// -------------------------------------------------------------------------

bool FControlApiService::HandleCamera(const FHttpServerRequest& Req, const FHttpResultCallback& Done)
{
	if (!Hooks.HasCamera || !Hooks.HasCamera())
	{
		RespondError(Done, 503, TEXT("no camera yet (CamSim is starting)"));
		return true;
	}
	if (Req.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		Respond(Done, 200, Hooks.CameraJson ? Hooks.CameraJson() : FString(TEXT("{}")));
		return true;
	}
	if (Req.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		bOwnsCamera = false;
		if (Hooks.SetCameraSuppressed) Hooks.SetCameraSuppressed(false);
		UE_LOG(LogCamSim, Log, TEXT("FControlApiService: camera released to CIGI"));
		Respond(Done, 200, TEXT("{\"camera_owned\":false}"));
		return true;
	}
	const auto P = ParseCamera(BodyText(Req));
	if (P.HasError())
	{
		RespondError(Done, 400, P.GetError().Message);
		return true;
	}
	if (!bOwnsCamera)
	{
		UE_LOG(LogCamSim, Log, TEXT("FControlApiService: the control API owns the camera; CIGI camera packets are ignored"));
	}
	bOwnsCamera = true;
	if (Hooks.SetCameraSuppressed) Hooks.SetCameraSuppressed(true);
	if (Hooks.ApplyCamera) Hooks.ApplyCamera(P.GetValue());
	Adapter.NoteCommand(GFrameCounter);
	Respond(Done, 200, Hooks.CameraJson ? Hooks.CameraJson() : FString(TEXT("{}")));
	return true;
}

// -------------------------------------------------------------------------
// Entities
// -------------------------------------------------------------------------

FString FControlApiService::EntitiesJson() const
{
	FString Out = TEXT("[");
	for (const uint16 Id : Adapter.GetIds())
	{
		if (Out.Len() > 1) Out += TEXT(",");
		CamSimFrames::FGeoPose Pose;
		const bool bSpawned = Hooks.EntityPose && Hooks.EntityPose(FControlApiAdapter::KeyFor(Id), Pose);
		Out += FString::Printf(TEXT("{\"id\":%u,\"type_id\":%u,\"spawned\":%s"), Id, Adapter.GetTypeId(Id), Bool(bSpawned));
		if (bSpawned)
		{
			const FRotator Hpr = CamSimFrames::EastSouthUpToCigi(CamSimFrames::NeuToEastSouthUp(Pose.Neu));
			Out += FString::Printf(TEXT(",\"lat\":%.9f,\"lon\":%.9f,\"alt\":%.4f,\"heading\":%.4f,\"pitch\":%.4f,\"roll\":%.4f"),
				Pose.Lat, Pose.Lon, Pose.Alt, FRotator::ClampAxis(Hpr.Yaw), Hpr.Pitch, Hpr.Roll);
		}
		Out += TEXT("}");
	}
	return Out + TEXT("]");
}

bool FControlApiService::HandleEntities(const FHttpServerRequest& Req, const FHttpResultCallback& Done)
{
	if (Req.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		Adapter.RemoveAll();
		Respond(Done, 200, TEXT("{\"entities\":[]}"));
		return true;
	}
	Respond(Done, 200, FString::Printf(TEXT("{\"entities\":%s}"), *EntitiesJson()));
	return true;
}

bool FControlApiService::HandleEntity(const FHttpServerRequest& Req, const FHttpResultCallback& Done)
{
	const uint32 Id = PathId(Req);
	if (Req.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		if (Id < 1 || Id > 65535)
		{
			RespondError(Done, 400, TEXT("entity id must be in 1..65535"));
		}
		else if (!Adapter.RemoveEntity(static_cast<uint16>(Id)))
		{
			RespondError(Done, 404, FString::Printf(TEXT("no API entity %u"), Id));
		}
		else
		{
			Respond(Done, 200, FString::Printf(TEXT("{\"id\":%u,\"removed\":true}"), Id));
		}
		return true;
	}
	const auto P = ParseEntity(Id, BodyText(Req));
	if (P.HasError())
	{
		RespondError(Done, 400, P.GetError().Message);
		return true;
	}
	FString Error;
	if (!Adapter.PutEntity(P.GetValue(), Error))
	{
		RespondError(Done, 400, Error);
		return true;
	}
	Respond(Done, 200, FString::Printf(TEXT("{\"id\":%u,\"type_id\":%u}"), Id, Adapter.GetTypeId(static_cast<uint16>(Id))));
	return true;
}

// -------------------------------------------------------------------------
// Environment
// -------------------------------------------------------------------------

bool FControlApiService::HandleEnvironment(const FHttpServerRequest& Req, const FHttpResultCallback& Done)
{
	const auto P = ParseEnvironment(BodyText(Req));
	if (P.HasError())
	{
		RespondError(Done, 400, P.GetError().Message);
		return true;
	}
	const FEnvironmentRequest& E = P.GetValue();
	bool bOk = true;
	if (E.Atmosphere) bOk &= Hooks.ApplyAtmosphere && Hooks.ApplyAtmosphere(*E.Atmosphere);
	if (E.Clouds)     bOk &= Hooks.ApplyWeather && Hooks.ApplyWeather(*E.Clouds);
	if (E.Beaufort || E.SeaDirectionDeg)
	{
		bOk &= Hooks.ApplySea && Hooks.ApplySea(E.Beaufort.Get(-1.0f), E.SeaDirectionDeg.Get(-1.0f));
	}
	Adapter.NoteCommand(GFrameCounter);
	if (!bOk)
	{
		RespondError(Done, 503, TEXT("the environment (or the ocean) is not available"));
		return true;
	}
	Respond(Done, 200, TEXT("{\"applied\":true}"));
	return true;
}

// -------------------------------------------------------------------------
// Capture
// -------------------------------------------------------------------------

bool FControlApiService::HandleCapture(const FHttpServerRequest& Req, const FHttpResultCallback& Done)
{
	const auto P = ParseCapture(BodyText(Req));
	if (P.HasError())
	{
		RespondError(Done, 400, P.GetError().Message);
		return true;
	}
	if (!Hooks.HasCamera || !Hooks.HasCamera() || !Hooks.SceneState || !Hooks.RequestCaptureFrame)
	{
		RespondError(Done, 503, TEXT("no camera yet (CamSim is starting)"));
		return true;
	}
	if (CaptureDone)
	{
		RespondError(Done, 409, TEXT("a capture is already in flight"));
		return true;
	}
	const FCaptureRequest& C = P.GetValue();
	const double Now = FPlatformTime::Seconds();
	CaptureDone = Done;
	CaptureJob.Reset();
	bCaptureWantDepth = C.bDepth;
	CaptureBeginSec = Now;
	CaptureFiredSec = 0.0;
	Gate.Begin(Now, C.SettleFrames.Get(Cfg.DefaultSettleFrames), C.TimeoutSec.Get(Cfg.CaptureTimeoutSec), Cfg.SettleLoadPct);
	return true;
}

void FControlApiService::FinishCapture(int32 Code, const FString& Body)
{
	FHttpResultCallback Done = MoveTemp(CaptureDone);
	CaptureDone = nullptr;
	CaptureJob.Reset();
	Gate.Reset();
	if (Done) Respond(Done, Code, Body);
}

void FControlApiService::Tick(double NowSec)
{
	const uint64 Serial = Adapter.GetCommandSerial();
	const bool bCommand = Serial != LastCommandSerial;
	LastCommandSerial = Serial;
	if (!CaptureDone) return;

	if (!CaptureJob)
	{
		FControlCaptureInputs In = Hooks.SceneState();
		In.NowSec = NowSec;
		In.bCommandThisTick = bCommand;
		switch (Gate.Update(In))
		{
		case FControlCaptureGate::EState::Fire:
		{
			CaptureFramesWaited = Gate.FramesWaited();
			CaptureJob = MakeShared<FControlCaptureJob, ESPMode::ThreadSafe>();
			CaptureJob->bWantDepth = bCaptureWantDepth;
			CaptureFiredSec = NowSec;
			Hooks.RequestCaptureFrame(CaptureJob.ToSharedRef());
			break;
		}
		case FControlCaptureGate::EState::TimedOut:
			FinishCapture(503, ErrorJson(FString::Printf(TEXT("capture timed out after %.0f s waiting for %s"),
				NowSec - CaptureBeginSec, *Gate.PendingReason())));
			break;
		default:
			break;
		}
		return;
	}

	if (!CaptureJob->bDone.Load(EMemoryOrder::SequentiallyConsistent))
	{
		if (NowSec - CaptureFiredSec > DeliveryTimeoutSec)
		{
			FinishCapture(503, ErrorJson(TEXT("the capture frame was not delivered (sensor off, terrain gate closed or readback stalled)")));
		}
		return;
	}

	const FControlCaptureJob& J = *CaptureJob;
	if (!J.Error.IsEmpty())
	{
		FinishCapture(503, ErrorJson(FString::Printf(TEXT("capture failed: %s"), *J.Error)));
		return;
	}
	FString Body;
	Body.Reserve(static_cast<int32>(J.Png.Num() * 4 / 3 + J.DepthPng.Num() * 4 / 3 + J.Record.Len() + 512));
	Body += FString::Printf(TEXT("{\"frame_id\":%llu,\"timestamp_us\":%llu,\"utc\":\"%s\",\"width\":%d,\"height\":%d,\"sensor_mode\":\"%s\","),
		J.FrameIdx, J.TimestampUs, *UtcIso(FSimClock::FromMicros(J.TimestampUs)), J.Width, J.Height,
		J.SensorMode == 1 ? TEXT("ir") : TEXT("eo"));
	Body += TEXT("\"image_png_b64\":\"");
	Body += FBase64::Encode(J.Png.GetData(), static_cast<uint32>(J.Png.Num()));
	Body += TEXT("\",");
	if (J.DepthPng.Num() > 0)
	{
		Body += TEXT("\"depth_png_b64\":\"");
		Body += FBase64::Encode(J.DepthPng.GetData(), static_cast<uint32>(J.DepthPng.Num()));
		Body += FString::Printf(TEXT("\",\"depth_far_plane_m\":%g,"), J.DepthFarPlaneM);
	}
	Body += TEXT("\"annotation\":");
	Body += J.Record.IsEmpty() ? FString(TEXT("null")) : J.Record;
	Body += FString::Printf(TEXT(",\"settle\":{\"frames_waited\":%d,\"wall_ms\":%.0f}}"),
		CaptureFramesWaited, (NowSec - CaptureBeginSec) * 1000.0);
	FinishCapture(200, Body);
}
