// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Thermal/LandCoverWindow.h"
#include "Tests/LandCoverTestTiles.h"

// CamSim.GPU.LandCover.*: the window's R8_UINT texture on the real RHI (ROADMAP 4B). Skipped under NullRHI.

using namespace CamSimLandCoverTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowUploadGpuTest, "CamSim.GPU.LandCover.WindowUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowUploadGpuTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
	const FString Dir = TempDir(TEXT("GpuUpload"));
	TArray<FTestTile> Tiles;
	for (int32 I = 754; I <= 756; ++I)
		for (int32 J = -2451; J <= -2448; ++J) Tiles.Add(Uniform(I, J, static_cast<uint8>(10 * (J + 2452) + (I - 753))));
	WriteTileDir(Dir, Tiles);
	FLandCoverWindow::FSettings S;
	S.Dir = Dir;
	S.Texels = 512;
	FLandCoverWindow W;
	W.Configure(S);
	W.Update(37.775, -122.45);
	W.FinishBuildForTest();
	const auto Data = W.GetCurrent();
	const auto Gpu = W.GetCurrentGpu();
	if (!TestTrue(TEXT("published"), Data.IsValid() && Gpu.IsValid())) return false;

	TArray<uint8> Read;
	bool bTexture = false;
	FIntVector Size = FIntVector::ZeroValue;
	EPixelFormat Format = PF_Unknown;
	ENQUEUE_RENDER_COMMAND(CamSimLandCoverUploadTest)([Gpu, &Read, &bTexture, &Size, &Format](FRHICommandListImmediate& RHICmdList)
	{
		bTexture = Gpu->Texture.IsValid();
		if (!bTexture) return;
		Size = Gpu->Texture->GetSizeXYZ();
		Format = Gpu->Texture->GetFormat();
		FRHIGPUTextureReadback Rb(TEXT("CamSimLandCoverUploadTest"));
		RHICmdList.Transition(FRHITransitionInfo(Gpu->Texture, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
		Rb.EnqueueCopy(RHICmdList, Gpu->Texture);
		RHICmdList.Transition(FRHITransitionInfo(Gpu->Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		if (!Rb.IsReady()) return;
		int32 Pitch = 0;
		const uint8* P = static_cast<const uint8*>(Rb.Lock(Pitch));
		Read.SetNumUninitialized(Size.X * Size.Y);
		for (int32 Y = 0; Y < Size.Y; ++Y) FMemory::Memcpy(Read.GetData() + Y * Size.X, P + static_cast<int64>(Y) * Pitch, Size.X);
		Rb.Unlock();
	});
	FlushRenderingCommands();
	if (!TestTrue(TEXT("texture created on the render thread"), bTexture)) return false;
	TestEqual(TEXT("width"), Size.X, 512);
	TestEqual(TEXT("height"), Size.Y, 512);
	TestEqual(TEXT("R8_UINT"), static_cast<int32>(Format), static_cast<int32>(PF_R8_UINT));
	if (!TestEqual(TEXT("read back"), Read.Num(), 512 * 512)) return false;
	int32 Mismatches = 0;
	for (int32 K = 0; K < Read.Num(); ++K) Mismatches += Read[K] != Data->Codes[K];
	TestEqual(TEXT("texel (x, y) = Codes[y * T + x], row 0 north, unflipped"), Mismatches, 0);
	AddInfo(FString::Printf(TEXT("512^2 window: %lld nonzero texels, %d mismatches"), Data->NonZeroTexels, Mismatches));
	return true;
}
