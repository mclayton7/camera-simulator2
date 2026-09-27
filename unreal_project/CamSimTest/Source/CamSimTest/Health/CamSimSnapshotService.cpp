// Copyright CamSim Contributors. All Rights Reserved.

#include "Health/CamSimSnapshotService.h"
#include "CamSimTest.h"
#include "HttpServerResponse.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Async/Async.h"

void FCamSimSnapshotService::Request(FHttpResultCallback OnComplete)
{
	checkSlow(IsInGameThread());
	Pending.Add({ MoveTemp(OnComplete), FPlatformTime::Seconds() });
}

void FCamSimSnapshotService::OfferFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	checkSlow(IsInGameThread());
	if (Pending.Num() == 0 || Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height) return;

	// Load on the game thread; the pool task only uses the module.
	IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	TSharedRef<FEncodeJob, ESPMode::ThreadSafe> Job = MakeShared<FEncodeJob, ESPMode::ThreadSafe>();
	InFlight.Add({ Job, MoveTemp(Pending) });
	Pending.Reset();

	Async(EAsyncExecution::ThreadPool, [&IWM, Job, Frame = TArray<FColor>(Pixels), Width, Height]()
	{
		TSharedPtr<IImageWrapper> Png = IWM.CreateImageWrapper(EImageFormat::PNG);
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
		auto Response = FHttpServerResponse::Create(FString(TEXT("{\"status\":\"no_frame\"}")), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::ServiceUnavail;
		Pending[I].OnComplete(MoveTemp(Response));
		Pending.RemoveAt(I);
	}
}
