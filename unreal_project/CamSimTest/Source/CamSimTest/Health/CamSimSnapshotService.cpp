// Copyright CamSim Contributors. All Rights Reserved.

#include "Health/CamSimSnapshotService.h"
#include "CamSimTest.h"
#include "HttpServerResponse.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Async/Async.h"

namespace
{
	TUniquePtr<FHttpServerResponse> ServiceUnavailable(const TCHAR* Status)
	{
		auto Response = FHttpServerResponse::Create(FString::Printf(TEXT("{\"status\":\"%s\"}"), Status), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::ServiceUnavail;
		return Response;
	}
}

FCamSimSnapshotService::~FCamSimSnapshotService()
{
	// A running encode keeps its own reference to the job and finishes harmlessly.
	for (FInFlight& F : InFlight)
	{
		for (FPendingRequest& Req : F.Waiting) { Req.OnComplete(ServiceUnavailable(TEXT("shutting_down"))); }
	}
	for (FPendingRequest& Req : Pending) { Req.OnComplete(ServiceUnavailable(TEXT("shutting_down"))); }
}

void FCamSimSnapshotService::Request(FHttpResultCallback OnComplete)
{
	checkSlow(IsInGameThread());
	Pending.Add({ MoveTemp(OnComplete), FPlatformTime::Seconds() });
}

void FCamSimSnapshotService::OfferFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	checkSlow(IsInGameThread());
	if (Pending.Num() == 0 || Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height) return;

	// Create the wrapper on the game thread: the pool task must not hold a
	// reference to the module, which can unload at exit while it runs.
	TSharedPtr<IImageWrapper> Png = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"))
		.CreateImageWrapper(EImageFormat::PNG);

	TSharedRef<FEncodeJob, ESPMode::ThreadSafe> Job = MakeShared<FEncodeJob, ESPMode::ThreadSafe>();
	InFlight.Add({ Job, MoveTemp(Pending) });
	Pending.Reset();

	Async(EAsyncExecution::ThreadPool, [Png = MoveTemp(Png), Job, Frame = TArray<FColor>(Pixels), Width, Height]() mutable
	{
		// A video frame is opaque. The primary view's final image carries alpha 0
		// (the old SceneCapture wrote 255), which viewers show as blank/white.
		for (FColor& P : Frame) { P.A = 255; }

		if (Png.IsValid() && Png->SetRaw(Frame.GetData(), Frame.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			const TArray64<uint8> Encoded = Png->GetCompressed(0);
			Job->Png = TArray<uint8>(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
		}
		Job->bDone.Store(true, EMemoryOrder::SequentiallyConsistent);  // Png visible to Tick()
	});
}

void FCamSimSnapshotService::Tick(double NowSec)
{
	checkSlow(IsInGameThread());

	for (int32 I = InFlight.Num() - 1; I >= 0; --I)
	{
		FInFlight& F = InFlight[I];
		if (!F.Job->bDone.Load(EMemoryOrder::SequentiallyConsistent)) continue;
		for (FPendingRequest& Req : F.Waiting)
		{
			TUniquePtr<FHttpServerResponse> Response = F.Job->Png.Num() > 0
				? FHttpServerResponse::Create(TArray<uint8>(F.Job->Png), TEXT("image/png"))
				: FHttpServerResponse::Error(EHttpServerResponseCodes::ServerError, TEXT("encode_failed"), TEXT("PNG encode failed"));
			Req.OnComplete(MoveTemp(Response));
		}
		InFlight.RemoveAt(I);
	}

	for (int32 I = Pending.Num() - 1; I >= 0; --I)
	{
		if (NowSec - Pending[I].RequestedAtSec < TimeoutSec) continue;
		Pending[I].OnComplete(ServiceUnavailable(TEXT("no_frame")));
		Pending.RemoveAt(I);
	}
}
