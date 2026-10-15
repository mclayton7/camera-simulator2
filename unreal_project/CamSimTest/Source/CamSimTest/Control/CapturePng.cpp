// Copyright CamSim Contributors. All Rights Reserved.

#include "Control/CapturePng.h"
#include "Encoder/Nv12.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

namespace CamSimControl
{
TArray64<uint8> EncodeNv12Png(const TArray<uint8>& Nv12, int32 Width, int32 Height)
{
	if (Width <= 0 || Height <= 0 || Nv12.Num() != CamSimNv12::NumBytes(Width, Height)) return {};
	IImageWrapperModule* Module = FModuleManager::GetModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
	if (!Module && IsInGameThread()) Module = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	if (!Module) return {};

	TArray<FColor> Bgra;
	CamSimNv12::ToBgra(Nv12.GetData(), Width, Height, Bgra);
	TSharedPtr<IImageWrapper> Png = Module->CreateImageWrapper(EImageFormat::PNG);
	if (!Png || !Png->SetRaw(Bgra.GetData(), Bgra.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8)) return {};
	return Png->GetCompressed(0);
}
}
