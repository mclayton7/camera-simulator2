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

	// Quantize float metres → uint16
	TArray<uint16> Pixels16;
	Pixels16.SetNumUninitialized(Width * Height);
	const float Scale = 65535.0f / DepthFarPlaneM;
	for (int32 i = 0; i < Width * Height; ++i)
	{
		const float Clamped = FMath::Clamp(DepthMetres[i], 0.0f, DepthFarPlaneM);
		Pixels16[i] = static_cast<uint16>(FMath::RoundToInt(Clamped * Scale));
	}

	TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule->CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper)
	{
		UE_LOG(LogCamSim, Warning, TEXT("FDepthMapWriter: failed to create PNG image wrapper"));
		return;
	}

	Wrapper->SetRaw(Pixels16.GetData(), Pixels16.Num() * sizeof(uint16),
	                Width, Height, ERGBFormat::Gray, 16);

	TArray64<uint8> PngData = Wrapper->GetCompressed(0);
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

void FDepthMapWriter::Close()
{
	bIsOpen = false;
	ImageWrapperModule = nullptr;
}
