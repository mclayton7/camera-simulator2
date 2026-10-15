// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/FDepthMapWriter.h"
#include "CamSimTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

bool FDepthMapWriter::Open(const FString& OutputDir, float InDepthFarPlaneM)
{
	if (bIsOpen) return true;

	DepthFarPlaneM = FMath::Max(InDepthFarPlaneM, 1.0f);

	DepthDir = FPaths::Combine(OutputDir, TEXT("depth"));
	IFileManager::Get().MakeDirectory(*DepthDir, /*Tree=*/true);

	// Load the image wrapper module on the game thread (not thread-safe to load later)
	ImageWrapperModule = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	bIsOpen = true;
	UE_LOG(LogCamSim, Log, TEXT("FDepthMapWriter: open -> %s (far=%.0fm)"), *DepthDir, DepthFarPlaneM);
	return true;
}

void FDepthMapWriter::WriteDepthFrame(
    const TArray<float>& DepthMetres,
    int32 Width, int32 Height, uint64 FrameIdx)
{
	if (!bIsOpen || !ImageWrapperModule || DepthMetres.Num() != Width * Height) return;

	const TArray64<uint8> PngData = EncodeDepthPng(DepthMetres, Width, Height, DepthFarPlaneM);
	if (PngData.IsEmpty())
	{
		UE_LOG(LogCamSim, Warning, TEXT("FDepthMapWriter: PNG compression failed for frame %llu"),
		       (unsigned long long)FrameIdx);
		return;
	}

	const FString FilePath = FPaths::Combine(
		DepthDir, FString::Printf(TEXT("depth_%08llu.png"), (unsigned long long)FrameIdx));

	if (!FFileHelper::SaveArrayToFile(PngData, *FilePath))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FDepthMapWriter: failed to write %s"), *FilePath);
	}
}

TArray64<uint8> FDepthMapWriter::EncodeDepthPng(const TArray<float>& DepthMetres, int32 Width, int32 Height, float FarPlaneM)
{
	if (Width <= 0 || Height <= 0 || DepthMetres.Num() != Width * Height) return {};
	// Loaded on the game thread by Open() or the control API before any task-thread call.
	IImageWrapperModule* Module = FModuleManager::GetModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
	if (!Module && IsInGameThread()) Module = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	if (!Module) return {};

	// Quantize float metres → uint16
	const float Far = FMath::Max(FarPlaneM, 1.0f);
	TArray<uint16> Pixels16;
	Pixels16.SetNumUninitialized(Width * Height);
	const float Scale = 65535.0f / Far;
	for (int32 i = 0; i < Width * Height; ++i)
	{
		const float Clamped = FMath::Clamp(DepthMetres[i], 0.0f, Far);
		Pixels16[i] = static_cast<uint16>(FMath::RoundToInt(Clamped * Scale));
	}

	TSharedPtr<IImageWrapper> Wrapper = Module->CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper || !Wrapper->SetRaw(Pixels16.GetData(), Pixels16.Num() * sizeof(uint16), Width, Height, ERGBFormat::Gray, 16))
	{
		return {};
	}
	return Wrapper->GetCompressed(0);
}

void FDepthMapWriter::Close()
{
	bIsOpen = false;
	ImageWrapperModule = nullptr;
}
