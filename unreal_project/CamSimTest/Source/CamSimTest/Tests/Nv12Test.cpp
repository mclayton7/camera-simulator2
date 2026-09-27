// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Encoder/Nv12.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNv12SplitTest, "CamSim.Encoder.Nv12.SplitToYuv420p",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNv12SplitTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 8, H = 4;
	TArray<uint8> Nv12; Nv12.SetNumUninitialized(CamSimNv12::NumBytes(W, H));
	for (int32 I = 0; I < W * H; ++I) Nv12[I] = static_cast<uint8>(I);
	for (int32 I = 0; I < W * H / 2; I += 2) { Nv12[W * H + I] = 100 + I; Nv12[W * H + I + 1] = 200 + I; }
	uint8 Y[W * H], U[W * H / 4], V[W * H / 4];
	CamSimNv12::SplitToYuv420p(Nv12.GetData(), W, H, Y, W, U, W / 2, V, W / 2);
	TestEqual(TEXT("Y copied"), FMemory::Memcmp(Y, Nv12.GetData(), W * H), 0);
	for (int32 I = 0; I < W * H / 4; ++I)
	{
		TestEqual(TEXT("U"), (int32)U[I], 100 + 2 * I);
		TestEqual(TEXT("V"), (int32)V[I], 200 + 2 * I);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNv12ToBgraTest, "CamSim.Encoder.Nv12.ToBgraLimitedRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FNv12ToBgraTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 4, H = 2;
	TArray<uint8> Nv12; Nv12.Init(128, CamSimNv12::NumBytes(W, H));
	Nv12[0] = 16; Nv12[1] = 235; Nv12[2] = 126;   // black, white, ~mid grey
	TArray<FColor> Out;
	CamSimNv12::ToBgra(Nv12.GetData(), W, H, Out);
	TestEqual(TEXT("black"), (int32)Out[0].R, 0);
	TestEqual(TEXT("white"), (int32)Out[1].G, 255);
	TestTrue(TEXT("grey"), FMath::Abs((int32)Out[2].B - 128) <= 1);
	TestEqual(TEXT("neutral chroma is grey"), (int32)Out[1].R, (int32)Out[1].B);
	return true;
}
