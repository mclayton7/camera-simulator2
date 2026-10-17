// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Environment/StarField.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStarFieldBinTest, "CamSim.Environment.StarField.BinConservesFlux",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStarFieldBinTest::RunTest(const FString&)
{
	TArray<FStar> Stars = { { 101.287, -16.716, -1.46, 0.0 }, { 279.234, 38.784, 0.03, 0.0 }, { 10.0, 89.5, 3.0, 1.1 } };
	const FStarFieldImage Img = FStarField::Bin(Stars, 256, 128);
	TestEqual(TEXT("size"), Img.Texels.Num(), 256 * 128);
	// Sum radiance x Omega_texel (luminance) = sum E.
	double Sum = 0.0;
	for (int32 Y = 0; Y < 128; ++Y)
	{
		const double Dec0 = 90.0 - 180.0 * Y / 128, Dec1 = 90.0 - 180.0 * (Y + 1) / 128;
		const double Omega = (2.0 * UE_DOUBLE_PI / 256) * (FMath::Sin(FMath::DegreesToRadians(Dec0)) - FMath::Sin(FMath::DegreesToRadians(Dec1)));
		for (int32 X = 0; X < 256; ++X)
		{
			const FLinearColor C = Img.Texels[Y * 256 + X].GetFloats();
			Sum += (0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B) * Omega;
		}
	}
	const double Expected = FStarField::StarLux(-1.46) + FStarField::StarLux(0.03) + FStarField::StarLux(3.0);
	TestTrue(TEXT("flux conserved 1%"), FMath::Abs(Sum / Expected - 1.0) < 0.01);
	TestTrue(TEXT("TotalLux"), FMath::Abs(Img.TotalLux / Expected - 1.0) < 1e-6);
	// Sirius (RA 101.287, Dec -16.716) lands in its texel.
	const int32 Sx = FMath::FloorToInt32(101.287 / 360.0 * 256), Sy = FMath::FloorToInt32((90.0 + 16.716) / 180.0 * 128);
	TestTrue(TEXT("Sirius texel lit"), Img.Texels[Sy * 256 + Sx].GetFloats().G > 0.0f);
	TestTrue(TEXT("Vega mag 0 ~ 2.56e-6 lux"), FMath::IsNearlyEqual(FStarField::StarLux(0.0), 2.56e-6, 0.05e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStarFieldMissingTest, "CamSim.Environment.StarField.MissingOrTruncated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStarFieldMissingTest::RunTest(const FString&)
{
	AddExpectedError(TEXT("star catalogue"), EAutomationExpectedErrorFlags::Contains, 2);
	TestTrue(TEXT("missing file -> empty"), FStarField::LoadDefault(TEXT("/nonexistent/bsc5.dat"), 64, 32).IsEmpty());
	const FString Pointer = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("bsc5_pointer.dat"));
	FFileHelper::SaveStringToFile(TEXT("version https://git-lfs.github.com/spec/v1\noid sha256:abc\nsize 1\n"), *Pointer);
	TestTrue(TEXT("LFS pointer -> empty"), FStarField::LoadDefault(Pointer, 64, 32).IsEmpty());
	IFileManager::Get().Delete(*Pointer);
	TArray<FStar> Stars; FString Err;
	TArray<uint8> Short; Short.SetNumZeroed(10);
	TestFalse(TEXT("short header"), FStarField::ParseBsc5(Short, Stars, Err));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStarFieldShippedTest, "CamSim.Environment.StarField.ShippedCatalogue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FStarFieldShippedTest::RunTest(const FString&)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FStarField::DefaultPath()))
	{
		AddError(TEXT("bsc5.dat missing - run git lfs pull"));
		return false;
	}
	TArray<FStar> Stars; FString Err;
	TestTrue(TEXT("parse ") + Err, FStarField::ParseBsc5(Bytes, Stars, Err));
	TestTrue(TEXT("~9,100 stars"), Stars.Num() > 9000 && Stars.Num() < 9200);
	const FStar* Brightest = nullptr;
	for (const FStar& S : Stars) if (!Brightest || S.Vmag < Brightest->Vmag) Brightest = &S;
	TestTrue(TEXT("brightest is Sirius"), Brightest && FMath::Abs(Brightest->RaDeg - 101.287) < 0.05 && FMath::Abs(Brightest->DecDeg + 16.716) < 0.05);
	return true;
}
